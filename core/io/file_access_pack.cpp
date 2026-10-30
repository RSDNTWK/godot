/**************************************************************************/
/*  file_access_pack.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "file_access_pack.h"

#include "core/crypto/crypto_core.h"
#include "core/io/file_access_encrypted.h"
#include "core/io/file_access_patched.h"
#include "core/io/marshalls.h"
#include "core/io/pck_directory_entry.h"
#include "core/io/pck_lzma.h"
#include "core/object/script_language.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/version.h"

#include <cstring>
namespace {

#ifdef TOOLS_ENABLED
constexpr bool pck_key_override_enabled = true;
#else
constexpr bool pck_key_override_enabled = false;
#endif

struct PCKLzmaCacheEntry {
	Vector<uint8_t> data;
	uint64_t size = 0;
	uint64_t last_use = 0;
};

struct PCKLzmaCacheKey {
	uint64_t identity = 0;
	uint64_t epoch = 0;
	bool encrypted = false;
	uint8_t encryption_domain[32] = {};

	bool operator==(const PCKLzmaCacheKey &p_other) const {
		return identity == p_other.identity && epoch == p_other.epoch && encrypted == p_other.encrypted &&
				memcmp(encryption_domain, p_other.encryption_domain, sizeof(encryption_domain)) == 0;
	}
	static uint32_t hash(const PCKLzmaCacheKey &p_key) {
		uint32_t h = hash_murmur3_one_64(p_key.identity);
		h = hash_murmur3_one_64(p_key.epoch, h);
		h = hash_murmur3_one_32(p_key.encrypted, h);
		// The byte fingerprint follows a bool and need not be word-aligned.
		// hash_murmur3_buffer reads native words, so give it aligned storage.
		uint32_t domain_words[8];
		memcpy(domain_words, p_key.encryption_domain, sizeof(domain_words));
		return hash_murmur3_buffer(domain_words, sizeof(domain_words), h);
	}
};

static Mutex pck_lzma_cache_mutex;
static HashMap<PCKLzmaCacheKey, PCKLzmaCacheEntry, PCKLzmaCacheKey> pck_lzma_cache;
static uint64_t pck_lzma_cache_epoch = 0;
static bool pck_lzma_cache_identity_exhausted = false;
static uint64_t pck_lzma_cache_total_size = 0;
static uint64_t pck_lzma_cache_use_counter = 0;

static uint64_t _get_pck_lzma_cache_budget_bytes() {
#if defined(ANDROID_ENABLED) || defined(IOS_ENABLED)
	return 16ULL * 1024ULL * 1024ULL;
#elif defined(WEB_ENABLED)
	return 8ULL * 1024ULL * 1024ULL;
#else
	return 256ULL * 1024ULL * 1024ULL;
#endif
}

// Entry identity and epoch share one process-wide monotonic sequence. A
// decode started before a mount mutation can never populate its successor.
static uint64_t _invalidate_pck_lzma_cache() {
	MutexLock lock(pck_lzma_cache_mutex);
	pck_lzma_cache.clear();
	pck_lzma_cache_total_size = 0;
	pck_lzma_cache_use_counter = 0;
	if (pck_lzma_cache_epoch == UINT64_MAX) {
		pck_lzma_cache_identity_exhausted = true;
	}
	if (pck_lzma_cache_identity_exhausted) {
		return 0;
	}
	return ++pck_lzma_cache_epoch;
}

static uint64_t _get_pck_lzma_cache_epoch() {
	MutexLock lock(pck_lzma_cache_mutex);
	return pck_lzma_cache_identity_exhausted ? 0 : pck_lzma_cache_epoch;
}

static bool _get_pck_lzma_cached_data(const PCKLzmaCacheKey &p_key, Vector<uint8_t> &r_data) {
	MutexLock lock(pck_lzma_cache_mutex);
	if (p_key.identity == 0 || p_key.epoch == 0 || p_key.epoch != pck_lzma_cache_epoch || pck_lzma_cache_identity_exhausted) {
		return false;
	}
	HashMap<PCKLzmaCacheKey, PCKLzmaCacheEntry, PCKLzmaCacheKey>::Iterator E = pck_lzma_cache.find(p_key);
	if (!E) {
		return false;
	}
	E->value.last_use = ++pck_lzma_cache_use_counter;
	r_data = E->value.data;
	return true;
}

static void _put_pck_lzma_cached_data(const PCKLzmaCacheKey &p_key, const Vector<uint8_t> &p_data) {
	const uint64_t budget = _get_pck_lzma_cache_budget_bytes();
	const uint64_t data_size = p_data.size();
	if (budget == 0 || data_size == 0 || data_size > (budget / 2)) {
		return;
	}

	MutexLock lock(pck_lzma_cache_mutex);
	if (p_key.identity == 0 || p_key.epoch == 0 || p_key.epoch != pck_lzma_cache_epoch || pck_lzma_cache_identity_exhausted) {
		return;
	}
	HashMap<PCKLzmaCacheKey, PCKLzmaCacheEntry, PCKLzmaCacheKey>::Iterator existing = pck_lzma_cache.find(p_key);
	if (existing) {
		if (existing->value.size <= pck_lzma_cache_total_size) {
			pck_lzma_cache_total_size -= existing->value.size;
		}
	}

	PCKLzmaCacheEntry &entry = pck_lzma_cache[p_key];
	entry.data = p_data;
	entry.size = data_size;
	entry.last_use = ++pck_lzma_cache_use_counter;
	pck_lzma_cache_total_size += data_size;

	while (pck_lzma_cache_total_size > budget && !pck_lzma_cache.is_empty()) {
		PCKLzmaCacheKey oldest_key;
		uint64_t oldest_use = (uint64_t)-1;
		for (const KeyValue<PCKLzmaCacheKey, PCKLzmaCacheEntry> &kv : pck_lzma_cache) {
			if (kv.value.last_use < oldest_use) {
				oldest_use = kv.value.last_use;
				oldest_key = kv.key;
			}
		}
		HashMap<PCKLzmaCacheKey, PCKLzmaCacheEntry, PCKLzmaCacheKey>::Iterator oldest = pck_lzma_cache.find(oldest_key);
		if (!oldest) {
			break;
		}
		if (oldest->value.size <= pck_lzma_cache_total_size) {
			pck_lzma_cache_total_size -= oldest->value.size;
		}
		pck_lzma_cache.erase(oldest_key);
	}
}

} // namespace

Error PCKEncryption::select_key(const Vector<uint8_t> &p_override, const uint8_t (&p_compiled)[32], bool p_allow_override, Vector<uint8_t> &r_key) {
	if (p_allow_override && !p_override.is_empty()) {
		if (p_override.size() != 32) {
			r_key.clear();
			return ERR_INVALID_PARAMETER;
		}
		constexpr uint8_t empty_key[32] = {};
		if (memcmp(p_compiled, empty_key, sizeof(empty_key)) == 0) {
			r_key = p_override;
			return OK;
		}
	}

	Vector<uint8_t> key;
	const Error allocation_error = key.resize(32);
	if (allocation_error != OK) {
		r_key.clear();
		return allocation_error;
	}
	memcpy(key.ptrw(), p_compiled, 32);
	r_key = key;
	return OK;
}

Error PackedData::add_pack(const String &p_path, bool p_replace_files, uint64_t p_offset, const Vector<uint8_t> &p_decryption_key) {
	for (int i = 0; i < sources.size(); i++) {
		if (sources[i]->try_open_pack(p_path, p_replace_files, p_offset, p_decryption_key)) {
			return OK;
		}
	}

	return ERR_FILE_UNRECOGNIZED;
}

void PackedData::add_path(const String &p_pkg_path, const String &p_path, uint64_t p_ofs, uint64_t p_size, uint64_t p_stored_size, const uint8_t *p_md5, PackSource *p_src, bool p_replace_files, bool p_encrypted, bool p_bundle, bool p_delta, bool p_lzma2, const String &p_salt) {
	String simplified_path = p_path.simplify_path().trim_prefix("res://");
	PathMD5 pmd5(simplified_path.md5_buffer());

	MutexLock lock(files_mutex);
	bool exists = files.has(pmd5);

	PackedFile pf;
	pf.encrypted = p_encrypted;
	pf.bundle = p_bundle;
	pf.delta = p_delta;
	pf.pack = p_pkg_path;
	pf.salt = p_salt;
	pf.offset = p_ofs;
	pf.size = p_size;
	pf.stored_size = p_stored_size;
	pf.lzma2 = p_lzma2;
	for (int i = 0; i < 16; i++) {
		pf.md5[i] = p_md5[i];
	}
	pf.src = p_src;

	if (p_delta) {
		pf.cache_identity = _invalidate_pck_lzma_cache();
		delta_patches[pmd5].push_back(pf);
	} else if (!exists || p_replace_files) {
		pf.cache_identity = _invalidate_pck_lzma_cache();
		files[pmd5] = pf;
		delta_patches[pmd5].clear();
	}

	if (!exists) {
		// Search for directory.
		PackedDir *cd = root;

		if (simplified_path.contains_char('/')) { // In a subdirectory.
			Vector<String> ds = simplified_path.get_base_dir().split("/");

			for (int j = 0; j < ds.size(); j++) {
				if (!cd->subdirs.has(ds[j])) {
					PackedDir *pd = memnew(PackedDir);
					pd->name = ds[j];
					pd->parent = cd;
					cd->subdirs[pd->name] = pd;
					cd = pd;
				} else {
					cd = cd->subdirs[ds[j]];
				}
			}
		}
		String filename = simplified_path.get_file();
		// Don't add as a file if the path points to a directory.
		if (!filename.is_empty()) {
			cd->files.insert(filename);
		}
	}
}

void PackedData::remove_path(const String &p_path) {
	String simplified_path = p_path.simplify_path().trim_prefix("res://");
	PathMD5 pmd5(simplified_path.md5_buffer());
	MutexLock lock(files_mutex);
	if (!files.has(pmd5) && !delta_patches.has(pmd5)) {
		return;
	}

	files.erase(pmd5);
	delta_patches.erase(pmd5);
	_invalidate_pck_lzma_cache();

	// Search for directory.
	PackedDir *cd = root;

	if (simplified_path.contains_char('/')) { // In a subdirectory.
		Vector<String> ds = simplified_path.get_base_dir().split("/");

		for (int j = 0; j < ds.size(); j++) {
			if (!cd->subdirs.has(ds[j])) {
				return; // Subdirectory does not exist, do not bother creating.
			} else {
				cd = cd->subdirs[ds[j]];
			}
		}
	}

	cd->files.erase(simplified_path.get_file());
}

void PackedData::add_pack_source(PackSource *p_source) {
	if (p_source != nullptr) {
		sources.push_back(p_source);
	}
}

uint8_t *PackedData::get_file_hash(const String &p_path) {
	String simplified_path = p_path.simplify_path().trim_prefix("res://");
	PathMD5 pmd5(simplified_path.md5_buffer());
	MutexLock lock(files_mutex);
	HashMap<PathMD5, PackedFile, PathMD5>::Iterator E = files.find(pmd5);
	if (!E) {
		return nullptr;
	}

	return E->value.md5;
}

Vector<PackedData::PackedFile> PackedData::get_delta_patches(const String &p_path) const {
	const PathMD5 pmd5 = _get_simplified_path(p_path);
	MutexLock lock(files_mutex);
	HashMap<PathMD5, Vector<PackedFile>, PathMD5>::ConstIterator E = delta_patches.find(pmd5);
	if (!E) {
		return Vector<PackedFile>();
	}

	return E->value;
}

bool PackedData::has_delta_patches(const String &p_path) const {
	const PathMD5 pmd5 = _get_simplified_path(p_path);
	MutexLock lock(files_mutex);
	HashMap<PathMD5, Vector<PackedFile>, PathMD5>::ConstIterator E = delta_patches.find(pmd5);
	if (!E) {
		return false;
	}

	return !E->value.is_empty();
}

HashSet<String> PackedData::get_file_paths() const {
	MutexLock lock(files_mutex);
	HashSet<String> file_paths;
	_get_file_paths(root, root->name, file_paths);
	return file_paths;
}

void PackedData::_get_file_paths(PackedDir *p_dir, const String &p_parent_dir, HashSet<String> &r_paths) const {
	for (const String &E : p_dir->files) {
		r_paths.insert(p_parent_dir.path_join(E));
	}

	for (const KeyValue<String, PackedDir *> &E : p_dir->subdirs) {
		_get_file_paths(E.value, p_parent_dir.path_join(E.key), r_paths);
	}
}

void PackedData::clear() {
	MutexLock lock(files_mutex);
	_invalidate_pck_lzma_cache();
	files.clear();
	delta_patches.clear();
	_free_packed_dirs(root);
	root = memnew(PackedDir);
}

PackedData::PackedData() {
	singleton = this;
	root = memnew(PackedDir);

	add_pack_source(memnew(PackedSourcePCK));
}

void PackedData::_free_packed_dirs(PackedDir *p_dir) {
	for (const KeyValue<String, PackedDir *> &E : p_dir->subdirs) {
		_free_packed_dirs(E.value);
	}
	memdelete(p_dir);
}

PackedData::~PackedData() {
	if (singleton == this) {
		singleton = nullptr;
	}

	for (int i = 0; i < sources.size(); i++) {
		memdelete(sources[i]);
	}
	_free_packed_dirs(root);
}

//////////////////////////////////////////////////////////////////

bool PackedSourcePCK::try_open_pack(const String &p_path, bool p_replace_files, uint64_t p_offset, const Vector<uint8_t> &p_decryption_key) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	if (f.is_null()) {
		return false;
	}
	const uint64_t physical_length = f->get_length();
	uint64_t active_length = physical_length;
	const auto seek_checked = [&](uint64_t p_position) {
		if (p_position > active_length) {
			return false;
		}
		f->seek(p_position);
		const Error error = f->get_error();
		return f->get_position() == p_position && (error == OK || error == ERR_FILE_EOF);
	};
	const auto read_bytes = [&](uint8_t *p_bytes, uint64_t p_length) {
		const uint64_t position = f->get_position();
		if (position > active_length || p_length > active_length - position) {
			return false;
		}
		if (p_length > 0 && f->get_buffer(p_bytes, p_length) != p_length) {
			return false;
		}
		const Error error = f->get_error();
		return f->get_position() == position + p_length && (error == OK || error == ERR_FILE_EOF);
	};
	const auto read_u32 = [&](uint32_t &r_value) {
		uint8_t bytes[4];
		if (!read_bytes(bytes, sizeof(bytes))) {
			return false;
		}
		r_value = decode_uint32(bytes);
		return true;
	};
	const auto read_u64 = [&](uint64_t &r_value) {
		uint8_t bytes[8];
		if (!read_bytes(bytes, sizeof(bytes))) {
			return false;
		}
		r_value = decode_uint64(bytes);
		return true;
	};

	uint32_t magic = 0;
	if (!seek_checked(p_offset) || !read_u32(magic)) {
		return false;
	}
	bool header_found = magic == PACK_HEADER_MAGIC;
	if (!header_found && p_offset != 0) {
		ERR_FAIL_V_MSG(false, "Loading self-contained executable with offset not supported.");
	}
	if (!header_found && physical_length >= 4) {
		const uint64_t embedded = OS::get_singleton()->get_embedded_pck_offset();
		if (embedded > 0 && embedded <= physical_length - 4) {
			const uint64_t probes = MIN(uint64_t(7), physical_length - 4 - embedded);
			for (uint64_t i = 0; i <= probes; i++) {
				if (!seek_checked(embedded + i) || !read_u32(magic)) {
					return false;
				}
				if (magic == PACK_HEADER_MAGIC) {
					header_found = true;
					break;
				}
			}
		}
	}
	if (!header_found && physical_length >= 12) {
		if (!seek_checked(physical_length - 4) || !read_u32(magic)) {
			return false;
		}
		if (magic == PACK_HEADER_MAGIC) {
			uint64_t pack_size;
			if (!seek_checked(physical_length - 12) || !read_u64(pack_size) || pack_size > physical_length - 12 ||
					!seek_checked(physical_length - 12 - pack_size) || !read_u32(magic)) {
				return false;
			}
			header_found = magic == PACK_HEADER_MAGIC;
		}
	}
	if (!header_found) {
		return false;
	}
	const uint64_t pack_start = f->get_position() - 4;
	uint32_t version, major, minor, patch, pack_flags;
	uint64_t file_base;
	if (!read_u32(version) || !read_u32(major) || !read_u32(minor) || !read_u32(patch) || !read_u32(pack_flags) || !read_u64(file_base)) {
		return false;
	}
	ERR_FAIL_COND_V_MSG(version != PACK_FORMAT_VERSION_V2 && version != PACK_FORMAT_VERSION_V3 &&
					version != PACK_FORMAT_VERSION_V4 && version != PACK_FORMAT_VERSION_V5,
			false, vformat("Pack version unsupported: %d.", version));
	ERR_FAIL_COND_V_MSG(major > GODOT_VERSION_MAJOR || (major == GODOT_VERSION_MAJOR && minor > GODOT_VERSION_MINOR),
			false, vformat("Pack created with a newer version of the engine: %d.%d.%d.", major, minor, patch));
	const bool encrypted_directory = pack_flags & PACK_DIR_ENCRYPTED;
	const bool sparse_bundle = pack_flags & PACK_SPARSE_BUNDLE;
	if (version >= PACK_FORMAT_VERSION_V3 || (pack_flags & PACK_REL_FILEBASE)) {
		if (file_base > UINT64_MAX - pack_start) {
			return false;
		}
		file_base += pack_start;
	}
	String salt;
	if (version >= PACK_FORMAT_VERSION_V3) {
		uint64_t directory_offset;
		if (!read_u64(directory_offset) || directory_offset > UINT64_MAX - pack_start) {
			return false;
		}
		if (sparse_bundle && encrypted_directory && version >= PACK_FORMAT_VERSION_V4) {
			uint8_t salt_bytes[32];
			if (!read_bytes(salt_bytes, sizeof(salt_bytes))) {
				return false;
			}
			if (salt.reserve(33) != OK) {
				return false;
			}
			salt.append_latin1(Span((const char *)salt_bytes, sizeof(salt_bytes)));
			if (salt.length() != 32) {
				return false;
			}
		}
		if (!seek_checked(directory_offset + pack_start)) {
			return false;
		}
	} else {
		uint32_t reserved;
		for (int i = 0; i < 16; i++) {
			if (!read_u32(reserved)) {
				return false;
			}
		}
	}
	uint32_t file_count;
	if (!read_u32(file_count)) {
		return false;
	}
	if (encrypted_directory) {
		Ref<FileAccessEncrypted> encrypted;
		encrypted.instantiate();
		ERR_FAIL_COND_V(encrypted.is_null(), false);
		Vector<uint8_t> key;
		if (PCKEncryption::select_key(p_decryption_key, script_encryption_key, pck_key_override_enabled, key) != OK ||
				encrypted->open_and_parse(f, key, FileAccessEncrypted::MODE_READ, false) != OK) {
			return false;
		}
		f = encrypted;
		active_length = f->get_length();
	}
	const uint64_t directory_position = f->get_position();
	const uint64_t minimum_entry = version >= PACK_FORMAT_VERSION_V5 ? 48 : 40;
	if (directory_position > active_length || file_count > INT_MAX || file_count > (active_length - directory_position) / minimum_entry) {
		return false;
	}
	Vector<PCKDirectory::Entry> pending;
	ERR_FAIL_COND_V(pending.resize(file_count) != OK, false);
	for (uint32_t i = 0; i < file_count; i++) {
		PCKDirectory::Entry &entry = pending.write[i];
		uint32_t path_length;
		if (!read_u32(path_length) || path_length >= INT_MAX) {
			return false;
		}
		const uint64_t position = f->get_position();
		const uint64_t metadata_bytes = minimum_entry - 4;
		if (position > active_length || active_length - position < metadata_bytes ||
				path_length > active_length - position - metadata_bytes) {
			return false;
		}
		CharString path_bytes;
		if (path_bytes.resize_uninitialized(uint64_t(path_length) + 1) != OK ||
				!read_bytes((uint8_t *)path_bytes.ptr(), path_length)) {
			return false;
		}
		path_bytes[path_length] = 0;
		if (entry.path.reserve(uint64_t(path_length) + 1) != OK ||
				entry.path.append_utf8(path_bytes.ptr(), int(path_length)) != OK ||
				!read_u64(entry.offset) || !read_u64(entry.size)) {
			return false;
		}
		entry.stored_size = entry.size;
		if (version >= PACK_FORMAT_VERSION_V5 && !read_u64(entry.stored_size)) {
			return false;
		}
		if (!read_bytes(entry.md5, sizeof(entry.md5)) || !read_u32(entry.flags)) {
			return false;
		}
		if (!(entry.flags & PACK_FILE_REMOVAL)) {
			if (entry.offset > UINT64_MAX - file_base) {
				return false;
			}
			entry.offset += file_base;
		}
	}

	// Parsing has no mount side effects. Commit in directory order only once
	// every entry is validated, under the existing recursive metadata lock.
	PackedData *packed = PackedData::get_singleton();
	ERR_FAIL_NULL_V(packed, false);
	MutexLock lock(packed->files_mutex);
	for (const PCKDirectory::Entry &entry : pending) {
		if (entry.flags & PACK_FILE_REMOVAL) {
			packed->remove_path(entry.path);
		} else {
			packed->add_path(p_path, entry.path, entry.offset, entry.size, entry.stored_size, entry.md5, this,
					p_replace_files, entry.flags & PACK_FILE_ENCRYPTED, sparse_bundle,
					entry.flags & PACK_FILE_DELTA, entry.flags & PACK_FILE_LZMA2, salt);
		}
	}
	return true;
}

Ref<FileAccess> PackedSourcePCK::get_file(const String &p_path, PackedData::PackedFile *p_file, const Vector<uint8_t> &p_decryption_key, const Vector<PackedData::PackedFile> *p_delta_snapshot) {
	Vector<PackedData::PackedFile> patches;
	if (p_delta_snapshot) {
		patches = *p_delta_snapshot;
	} else if (PackedData::get_singleton()) {
		patches = PackedData::get_singleton()->get_delta_patches(p_path);
	}
	Ref<FileAccess> file(memnew(FileAccessPack(p_path, *p_file, p_decryption_key)));
	// Preserve the initialization error for PackedData::try_open_path.
	if (!file->is_open()) {
		return file;
	}

	if (!patches.is_empty()) {
		Ref<FileAccessPatched> file_patched;
		file_patched.instantiate();
		Error err = file_patched->open_custom(file, p_decryption_key, &patches);
		ERR_FAIL_COND_V(err != OK, Ref<FileAccess>());
		file = file_patched;
	}

	return file;
}

//////////////////////////////////////////////////////////////////

bool PackedSourceDirectory::try_open_pack(const String &p_path, bool p_replace_files, uint64_t p_offset, const Vector<uint8_t> &p_decryption_key) {
	// Load with offset feature only supported for PCK files.
	ERR_FAIL_COND_V_MSG(p_offset != 0, false, "Invalid PCK data. Note that loading files with a non-zero offset isn't supported with directories.");

	if (p_path != "res://") {
		return false;
	}
	add_directory(p_path, p_replace_files);
	return true;
}

Ref<FileAccess> PackedSourceDirectory::get_file(const String &p_path, PackedData::PackedFile *p_file, const Vector<uint8_t> &p_decryption_key, const Vector<PackedData::PackedFile> *p_delta_snapshot) {
	Ref<FileAccess> ret = FileAccess::create_for_path(p_path);
	ret->reopen(p_path, FileAccess::READ);
	return ret;
}

void PackedSourceDirectory::add_directory(const String &p_path, bool p_replace_files) {
	Ref<DirAccess> da = DirAccess::open(p_path);
	if (da.is_null()) {
		return;
	}
	da->set_include_hidden(true);

	for (const String &file_name : da->get_files()) {
		String file_path = p_path.path_join(file_name);
		uint8_t md5[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
		PackedData::get_singleton()->add_path(p_path, file_path, 0, 0, 0, md5, this, p_replace_files, false, false, false, false);
	}

	for (const String &sub_dir_name : da->get_directories()) {
		String sub_dir_path = p_path.path_join(sub_dir_name);
		add_directory(sub_dir_path, p_replace_files);
	}
}

//////////////////////////////////////////////////////////////////

Error FileAccessPack::open_internal(const String &p_path, int p_mode_flags) {
	ERR_PRINT("Can't open pack-referenced file.");
	return ERR_UNAVAILABLE;
}

bool FileAccessPack::is_open() const {
	if (open_error != OK) {
		return false;
	}
	if (use_cached_data) {
		return true;
	}
	if (f.is_valid()) {
		return f->is_open();
	} else {
		return false;
	}
}

void FileAccessPack::seek(uint64_t p_position) {
	ERR_FAIL_COND_MSG(!is_open(), "File must be opened before use.");
	if (p_position > pf.size) {
		pos = p_position;
		eof = true;
		read_error = ERR_FILE_EOF;
		return;
	}
	if (!use_cached_data) {
		if (p_position > UINT64_MAX - off) {
			read_error = ERR_INVALID_PARAMETER;
			ERR_FAIL_MSG("Pack seek offset overflows.");
		}
		f->seek(off + p_position);
		const Error error = f->get_error();
		if (error != OK && error != ERR_FILE_EOF) {
			read_error = error;
			return;
		}
		if (f->get_position() != off + p_position) {
			read_error = ERR_FILE_CANT_READ;
			return;
		}
	}
	pos = p_position;
	eof = false;
	read_error = OK;
}

void FileAccessPack::seek_end(int64_t p_position) {
	ERR_FAIL_COND_MSG(!is_open(), "File must be opened before use.");
	if (p_position < 0) {
		const uint64_t distance = uint64_t(-(p_position + 1)) + 1;
		if (distance > pf.size) {
			read_error = ERR_INVALID_PARAMETER;
			ERR_FAIL_MSG("Cannot seek before the start of a pack entry.");
		}
		seek(pf.size - distance);
	} else {
		if (uint64_t(p_position) > UINT64_MAX - pf.size) {
			read_error = ERR_INVALID_PARAMETER;
			ERR_FAIL_MSG("Pack seek offset overflows.");
		}
		seek(pf.size + uint64_t(p_position));
	}
}

uint64_t FileAccessPack::get_position() const {
	return pos;
}

uint64_t FileAccessPack::get_length() const {
	return pf.size;
}

bool FileAccessPack::eof_reached() const {
	return eof;
}

uint64_t FileAccessPack::get_buffer(uint8_t *p_dst, uint64_t p_length) const {
	if (!is_open()) {
		if (open_error == OK) {
			read_error = ERR_FILE_CANT_READ;
		}
		ERR_FAIL_V_MSG(0, "File must be opened before use.");
	}
	if (!p_dst && p_length > 0) {
		read_error = ERR_INVALID_PARAMETER;
		ERR_FAIL_V(0);
	}
	if (p_length == 0 || eof) {
		return 0;
	}
	if (pos >= pf.size) {
		eof = true;
		read_error = ERR_FILE_EOF;
		return 0;
	}

	const uint64_t to_read = MIN(p_length, MIN(pf.size - pos, uint64_t(SIZE_MAX)));
	uint64_t read = to_read;
	if (use_cached_data) {
		memcpy(p_dst, cached_data.ptr() + pos, to_read);
	} else {
		read = f->get_buffer(p_dst, to_read);
		if (read > to_read) {
			const Error error = f->get_error();
			read_error = error != OK && error != ERR_FILE_EOF ? error : ERR_FILE_CANT_READ;
			return 0;
		}
		const Error error = f->get_error();
		if (error != OK && error != ERR_FILE_EOF) {
			read_error = error;
		} else if (read < to_read) {
			if (error == ERR_FILE_EOF || f->eof_reached()) {
				eof = true;
				read_error = ERR_FILE_EOF;
			} else if (read == 0) {
				read_error = ERR_FILE_CANT_READ;
			}
		}
	}
	pos += read;
	if (read == to_read && pos == pf.size && p_length > to_read) {
		eof = true;
		if (read_error == OK) {
			read_error = ERR_FILE_EOF;
		}
	}
	return read;
}

void FileAccessPack::set_big_endian(bool p_big_endian) {
	ERR_FAIL_COND_MSG(!is_open(), "File must be opened before use.");

	FileAccess::set_big_endian(p_big_endian);
	if (!use_cached_data) {
		f->set_big_endian(p_big_endian);
	}
}

Error FileAccessPack::get_error() const {
	if (open_error != OK) {
		return open_error;
	}
	if (read_error != OK) {
		return read_error;
	}
	return eof ? ERR_FILE_EOF : OK;
}

void FileAccessPack::flush() {
	ERR_FAIL();
}

bool FileAccessPack::store_buffer(const uint8_t *p_src, uint64_t p_length) {
	ERR_FAIL_V(false);
}

bool FileAccessPack::file_exists(const String &p_name) {
	return false;
}

void FileAccessPack::close() {
	open_error = ERR_UNCONFIGURED;
	read_error = OK;
	pos = 0;
	off = 0;
	eof = false;
	f = Ref<FileAccess>();
	cached_data.clear();
	use_cached_data = false;
}

FileAccessPack::FileAccessPack(const Ref<FileAccess> &p_base, uint64_t p_offset, uint64_t p_length) :
		pf{}, f(p_base) {
	path = p_base->get_path();
	pf.size = p_length;
	pf.stored_size = p_length;
	off = p_offset;
	open_error = OK;
	FileAccess::set_big_endian(p_base->is_big_endian());
}

FileAccessPack::FileAccessPack(const String &p_path, const PackedData::PackedFile &p_file, const Vector<uint8_t> &p_decryption_key) {
	path = p_path;
	pf = p_file;
	open_error = _open_pack(p_decryption_key);
	if (open_error != OK) {
		f.unref();
		cached_data.clear();
		use_cached_data = false;
	}
}

Error FileAccessPack::_open_pack(const Vector<uint8_t> &p_decryption_key) {
	PCKLzmaCacheKey cache_key;
	if (pf.lzma2 && pf.cache_identity != 0) {
		cache_key.identity = pf.cache_identity;
		cache_key.epoch = _get_pck_lzma_cache_epoch();
		cache_key.encrypted = pf.encrypted;
	}
	const String &p_path = path;
	if (pf.bundle) {
		String simplified_path = p_path.simplify_path();
		String path_to_load = simplified_path;
#ifdef TOOLS_ENABLED
		if (!pf.salt.is_empty()) {
			path_to_load = pf.pack.get_base_dir().path_join((simplified_path + pf.salt).sha256_text());
		} else {
			path_to_load = pf.pack.get_base_dir().path_join(simplified_path.replace("res://", ""));
		}
#else
		if (!pf.salt.is_empty()) {
			path_to_load = "res://" + (simplified_path + pf.salt).sha256_text();
		}
#endif
		Error err = OK;
		f = FileAccess::open(path_to_load, FileAccess::READ | FileAccess::SKIP_PACK, &err);
		ERR_FAIL_COND_V_MSG(err != OK, err, vformat(R"(Can't open pack-referenced file "%s" from sparse pack "%s" due to error "%s".)", simplified_path, pf.pack, error_names[err]));
		off = 0; // For the sparse pack offset is always zero.
	} else {
		Error err = OK;
		f = FileAccess::open(pf.pack, FileAccess::READ, &err);
		ERR_FAIL_COND_V_MSG(err != OK, err, vformat(R"(Can't open pack-referenced file "%s" from pack "%s" due to error "%s".)", p_path, pf.pack, error_names[err]));
		f->seek(pf.offset);
		const Error seek_error = f->get_error();
		ERR_FAIL_COND_V(seek_error != OK, seek_error);
		ERR_FAIL_COND_V_MSG(f->get_position() != pf.offset, ERR_FILE_CANT_READ, "Failed to seek to pack entry.");
		off = pf.offset;
	}

	// Version 5 LZMA2 metadata describes physical ordinary entries, but
	// plaintext compressed bytes in sparse bundles. Bound the encrypted view
	// before the common parser validates its inner length and padding.
	if (pf.encrypted && pf.lzma2) {
		uint64_t physical_size = pf.stored_size;
		if (pf.bundle) {
			ERR_FAIL_COND_V(pf.stored_size > INT32_MAX, ERR_FILE_CORRUPT);
			const uint64_t padding = (16 - pf.stored_size % 16) % 16;
			physical_size = 40 + pf.stored_size + padding;
		} else {
			ERR_FAIL_COND_V(physical_size > uint64_t(INT32_MAX) + 40 + 15, ERR_FILE_CORRUPT);
		}
		const uint64_t backing_size = f->get_length();
		ERR_FAIL_COND_V(off > backing_size || physical_size > backing_size - off, ERR_FILE_CORRUPT);
		f = Ref<FileAccess>(memnew(FileAccessPack(f, off, physical_size)));
		off = 0;
	}

	if (pf.encrypted) {
		Ref<FileAccessEncrypted> fae;
		fae.instantiate();
		ERR_FAIL_COND_V_MSG(fae.is_null(), ERR_OUT_OF_MEMORY, vformat(R"(Can't open encrypted pack-referenced file "%s" from pack "%s".)", p_path, pf.pack));

		Vector<uint8_t> key;
		const Error key_error = PCKEncryption::select_key(p_decryption_key, script_encryption_key, pck_key_override_enabled, key);
		ERR_FAIL_COND_V_MSG(key_error != OK, key_error, "Can't prepare encrypted pack entry key.");

		if (pf.lzma2 && cache_key.identity != 0 && CryptoCore::sha256(key.ptr(), key.size(), cache_key.encryption_domain) != OK) {
			// A cache-key failure must not alter verified file decoding.
			cache_key.identity = 0;
		}

		Error err = fae->open_and_parse(f, key, FileAccessEncrypted::MODE_READ, false);
		ERR_FAIL_COND_V_MSG(err != OK, err, vformat(R"(Can't open encrypted pack-referenced file "%s" from pack "%s" due to error "%s".)", p_path, pf.pack, error_names[err]));
		f = fae;
		off = 0;
	}
	if (pf.lzma2) {
		const uint64_t compressed_size = pf.encrypted ? f->get_length() : pf.stored_size;
		ERR_FAIL_COND_V(pf.encrypted && pf.bundle && compressed_size != pf.stored_size, ERR_FILE_CORRUPT);
		ERR_FAIL_COND_V_MSG(compressed_size > INT32_MAX, ERR_FILE_CORRUPT, vformat(R"(Compressed pack-referenced file "%s" is too large to decode in memory.)", p_path));
		if (!_get_pck_lzma_cached_data(cache_key, cached_data)) {
			Vector<uint8_t> compressed_data;
			const Error allocation_error = compressed_data.resize(compressed_size);
			ERR_FAIL_COND_V_MSG(allocation_error != OK, allocation_error, "Failed to allocate compressed pack input buffer.");
			if (compressed_size > 0) {
				const uint64_t read_size = f->get_buffer(compressed_data.ptrw(), compressed_size);
				const Error input_error = f->get_error();
				ERR_FAIL_COND_V(input_error != OK && input_error != ERR_FILE_EOF, input_error);
				ERR_FAIL_COND_V_MSG(read_size != compressed_size, ERR_FILE_CORRUPT, vformat(R"(Failed to read compressed payload for "%s" from pack "%s".)", p_path, pf.pack));
			}
			const Error err = decompress_lzma2(compressed_data.ptr(), compressed_data.size(), pf.size, cached_data);
			ERR_FAIL_COND_V_MSG(err != OK, err, vformat(R"(Failed to decompress LZMA2 payload for "%s" from pack "%s".)", p_path, pf.pack));
			_put_pck_lzma_cached_data(cache_key, cached_data);
		}
		use_cached_data = true;
		f.unref();
		off = 0;
	}
	return OK;
}

//////////////////////////////////////////////////////////////////////////////////
// DIR ACCESS
//////////////////////////////////////////////////////////////////////////////////

Error DirAccessPack::list_dir_begin() {
	list_dirs.clear();
	list_files.clear();

	for (const KeyValue<String, PackedData::PackedDir *> &E : current->subdirs) {
		list_dirs.push_back(E.key);
	}

	for (const String &E : current->files) {
		list_files.push_back(E);
	}

	return OK;
}

String DirAccessPack::get_next() {
	if (list_dirs.size()) {
		cdir = true;
		String d = list_dirs.front()->get();
		list_dirs.pop_front();
		return d;
	} else if (list_files.size()) {
		cdir = false;
		String f = list_files.front()->get();
		list_files.pop_front();
		return f;
	} else {
		return String();
	}
}

bool DirAccessPack::current_is_dir() const {
	return cdir;
}

bool DirAccessPack::current_is_hidden() const {
	return false;
}

void DirAccessPack::list_dir_end() {
	list_dirs.clear();
	list_files.clear();
}

int DirAccessPack::get_drive_count() {
	return 0;
}

String DirAccessPack::get_drive(int p_drive) {
	return "";
}

PackedData::PackedDir *DirAccessPack::_find_dir(const String &p_dir) {
	String nd = p_dir.replace_char('\\', '/');

	// Special handling since simplify_path() will forbid it
	if (p_dir == "..") {
		return current->parent;
	}

	bool absolute = false;
	if (nd.begins_with("res://")) {
		nd = nd.replace_first("res://", "");
		absolute = true;
	}

	nd = nd.simplify_path();

	if (nd.is_empty()) {
		nd = ".";
	}

	if (nd.begins_with("/")) {
		nd = nd.replace_first("/", "");
		absolute = true;
	}

	Vector<String> paths = nd.split("/");

	PackedData::PackedDir *pd;

	if (absolute) {
		pd = PackedData::get_singleton()->root;
	} else {
		pd = current;
	}

	for (int i = 0; i < paths.size(); i++) {
		const String &p = paths[i];
		if (p == ".") {
			continue;
		} else if (p == "..") {
			if (pd->parent) {
				pd = pd->parent;
			}
		} else if (pd->subdirs.has(p)) {
			pd = pd->subdirs[p];

		} else {
			return nullptr;
		}
	}

	return pd;
}

Error DirAccessPack::change_dir(String p_dir) {
	PackedData::PackedDir *pd = _find_dir(p_dir);
	if (pd) {
		current = pd;
		return OK;
	} else {
		return ERR_INVALID_PARAMETER;
	}
}

String DirAccessPack::get_current_dir(bool p_include_drive) const {
	PackedData::PackedDir *pd = current;
	String p = current->name;

	while (pd->parent) {
		pd = pd->parent;
		p = pd->name.path_join(p);
	}

	return "res://" + p;
}

bool DirAccessPack::file_exists(String p_file) {
	PackedData::PackedDir *pd = _find_dir(p_file.get_base_dir());
	if (!pd) {
		return false;
	}
	return pd->files.has(p_file.get_file());
}

bool DirAccessPack::dir_exists(String p_dir) {
	return _find_dir(p_dir) != nullptr;
}

Error DirAccessPack::make_dir(String p_dir) {
	return ERR_UNAVAILABLE;
}

Error DirAccessPack::rename(String p_from, String p_to) {
	return ERR_UNAVAILABLE;
}

Error DirAccessPack::remove(String p_name) {
	return ERR_UNAVAILABLE;
}

uint64_t DirAccessPack::get_space_left() {
	return 0;
}

String DirAccessPack::get_filesystem_type() const {
	return "PCK";
}

DirAccessPack::DirAccessPack() {
	current = PackedData::get_singleton()->root;
}
