/**************************************************************************/
/*  pck_compression_cache.cpp                                             */
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

#include "pck_compression_cache.h"

#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

#include <cstring>

namespace {
constexpr uint32_t CACHE_MAGIC = 0x434b4350;
constexpr uint32_t CACHE_VERSION = 1;
constexpr uint64_t HEADER_SIZE = 92;

bool valid_key(const String &p_key) {
	return p_key.length() == 64 && p_key.is_valid_hex_number(false);
}
} // namespace

PCKCompressionCache::PCKCompressionCache(const String &p_directory, uint64_t p_limit) :
		directory(p_directory), limit(p_limit) {
	if (directory.is_empty() || limit < HEADER_SIZE || DirAccess::make_dir_recursive_absolute(directory) != OK) {
		return;
	}
	Ref<DirAccess> dir = DirAccess::open(directory);
	if (dir.is_null()) {
		return;
	}
	for (const String &name : dir->get_files()) {
		if (name.ends_with(".tmp")) {
			const Vector<String> parts = name.split(".");
			if (parts.size() == 5 && valid_key(parts[0]) && parts[1] == "lzma2" && parts[2].is_valid_int() && parts[3].is_valid_int() && !OS::get_singleton()->is_process_running(parts[2].to_int())) {
				DirAccess::remove_absolute(directory.path_join(name));
			}
			continue;
		}
		if (!name.ends_with(".lzma2") || !valid_key(name.trim_suffix(".lzma2"))) {
			continue;
		}
		const String path = directory.path_join(name);
		Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
		if (file.is_valid()) {
			entries.push_back({ path, file->get_length(), FileAccess::get_modified_time(path) });
			disk_bytes += file->get_length();
		}
	}
	entries.sort();
	available = true;
	_make_room(0);
}

void PCKCompressionCache::_remove(const String &p_path) {
	if (DirAccess::remove_absolute(p_path) != OK && FileAccess::exists(p_path)) {
		return;
	}
	for (int i = 0; i < entries.size(); i++) {
		if (entries[i].path == p_path) {
			disk_bytes -= entries[i].size;
			entries.remove_at(i);
			break;
		}
	}
}

bool PCKCompressionCache::_make_room(uint64_t p_bytes) {
	if (p_bytes > limit) {
		return false;
	}
	while (disk_bytes > limit - p_bytes) {
		if (entries.is_empty()) {
			return false;
		}
		const uint64_t old_size = disk_bytes;
		_remove(entries[0].path);
		if (disk_bytes == old_size) {
			return false;
		}
	}
	return true;
}

String PCKCompressionCache::make_key(const Vector<uint8_t> &p_input, const PCKLzmaOptions &p_options) {
	// Bump this implementation tag when changing the encoder or its effective settings.
	const CharString settings = vformat("pck-lzma2-sdk2604-memory-v1:%d:%d:%d:%d:%d:", p_options.compression_level, p_options.dictionary_size_mb, p_options.word_size, p_options.threads, p_options.memory_usage_percent).utf8();
	CryptoCore::SHA256Context hash;
	uint8_t digest[32];
	if (hash.start() != OK || hash.update(reinterpret_cast<const uint8_t *>(settings.get_data()), settings.length()) != OK || hash.update(p_input.ptr(), p_input.size()) != OK || hash.finish(digest) != OK) {
		return String();
	}
	return String::hex_encode_buffer(digest, sizeof(digest));
}

PCKCompressionCache::Result PCKCompressionCache::load(const String &p_key, const Vector<uint8_t> &p_input, Vector<uint8_t> &r_compressed) {
	r_compressed.clear();
	if (!available || !valid_key(p_key) || p_input.is_empty() || p_input.size() > INT32_MAX) {
		return MISS;
	}
	const String path = directory.path_join(p_key + ".lzma2");
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
	if (file.is_null()) {
		return MISS;
	}
	bool valid = file->get_length() >= HEADER_SIZE;
	valid = valid && file->get_32() == CACHE_MAGIC && file->get_32() == CACHE_VERSION;
	valid = valid && file->get_64() == uint64_t(p_input.size());
	const uint64_t stored = valid ? file->get_64() : 0;
	const uint32_t flags = valid ? file->get_32() : 0;
	uint8_t key[64];
	const CharString expected_key = p_key.ascii();
	valid = valid && file->get_buffer(key, sizeof(key)) == sizeof(key) && memcmp(key, expected_key.get_data(), sizeof(key)) == 0;
	valid = valid && stored <= uint64_t(p_input.size()) && file->get_length() == HEADER_SIZE + stored;
	if (valid && flags == RAW && stored == 0) {
		return RAW; // Skipping ineffective compression still stores the original input normally.
	}
	if (valid && flags == COMPRESSED && stored > 1 && stored < uint64_t(p_input.size())) {
		r_compressed = file->get_buffer(stored);
		Vector<uint8_t> decoded;
		// Cache data never bypasses validation against this export's exact original bytes.
		valid = r_compressed.size() == int64_t(stored) && r_compressed[0] <= 40 && decompress_lzma2(r_compressed.ptr(), r_compressed.size(), p_input.size(), decoded) == OK && decoded == p_input;
		if (valid) {
			return COMPRESSED;
		}
	}
	file.unref();
	r_compressed.clear();
	_remove(path);
	return MISS;
}

void PCKCompressionCache::store(const String &p_key, const Vector<uint8_t> &p_input, const Vector<uint8_t> &p_candidate) {
	if (!available || !valid_key(p_key) || p_input.is_empty() || p_input.size() > INT32_MAX) {
		return;
	}
	const bool compressed = p_candidate.size() > 1 && p_candidate.size() < p_input.size();
	const uint64_t stored = compressed ? p_candidate.size() : 0;
	const String path = directory.path_join(p_key + ".lzma2");
	if (FileAccess::exists(path) || !_make_room(HEADER_SIZE + stored)) {
		return;
	}
	const String temporary = path + "." + itos(OS::get_singleton()->get_process_id()) + "." + uitos(OS::get_singleton()->get_ticks_usec()) + ".tmp";
	Ref<FileAccess> file = FileAccess::open(temporary, FileAccess::WRITE);
	if (file.is_null()) {
		return;
	}
	file->store_32(CACHE_MAGIC);
	file->store_32(CACHE_VERSION);
	file->store_64(p_input.size());
	file->store_64(stored);
	file->store_32(compressed ? COMPRESSED : RAW);
	const CharString key = p_key.ascii();
	file->store_buffer(reinterpret_cast<const uint8_t *>(key.get_data()), 64);
	if (compressed) {
		file->store_buffer(p_candidate.ptr(), stored);
	}
	file->flush();
	const bool written = file->get_error() == OK && file->get_length() == HEADER_SIZE + stored;
	file.unref();
	if (!written || DirAccess::rename_absolute(temporary, path) != OK) {
		DirAccess::remove_absolute(temporary);
		return;
	}
	entries.push_back({ path, HEADER_SIZE + stored, FileAccess::get_modified_time(path) });
	disk_bytes += HEADER_SIZE + stored;
}
