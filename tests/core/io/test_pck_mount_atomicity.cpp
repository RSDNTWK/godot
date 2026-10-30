/**************************************************************************/
/*  test_pck_mount_atomicity.cpp                                          */
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

#include "tests/test_macros.h"
TEST_FORCE_LINK(test_pck_mount_atomicity)
#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access_encrypted.h"
#include "core/io/file_access_memory.h"
#include "core/io/file_access_pack.h"
#include "core/io/marshalls.h"
#include "core/io/pck_directory_entry.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "tests/core/io/allocation_test_helpers.h"
#include "tests/test_utils.h"

#include <cstring>

namespace TestPCKMountAtomicity {
struct Access {
	static PackedData::PackedFile entry(const String &p_path) {
		PackedData *data = PackedData::get_singleton();
		MutexLock lock(data->files_mutex);
		const PackedData::PackedFile *entry = data->files.getptr(data->_get_simplified_path(p_path));
		REQUIRE(entry != nullptr);
		return *entry;
	}
};
struct QuietErrors {
	bool previous = CoreGlobals::print_error_enabled;
	QuietErrors() { CoreGlobals::print_error_enabled = false; }
	~QuietErrors() { CoreGlobals::print_error_enabled = previous; }
};
static void u32(Vector<uint8_t> &r_bytes, uint32_t p_value) {
	const int64_t offset = r_bytes.size();
	REQUIRE(r_bytes.resize(offset + 4) == OK);
	encode_uint32(p_value, r_bytes.ptrw() + offset);
}
static void u64(Vector<uint8_t> &r_bytes, uint64_t p_value) {
	const int64_t offset = r_bytes.size();
	REQUIRE(r_bytes.resize(offset + 8) == OK);
	encode_uint64(p_value, r_bytes.ptrw() + offset);
}
struct Entry {
	String name;
	String value;
	uint32_t flags = 0;
};
struct Frame {
	Vector<uint8_t> bytes;
	Vector<uint64_t> entry_positions;
	uint64_t directory_end = 0;
};
static Vector<uint8_t> encrypted_directory(const Vector<uint8_t> &p_plain) {
	Error error;
	Ref<FileAccess> backing = FileAccess::create_temp(FileAccess::WRITE_READ, "n036_directory", "bin", false, &error);
	REQUIRE(error == OK);
	REQUIRE(backing.is_valid());
	Vector<uint8_t> key;
	REQUIRE(key.resize(32) == OK);
	memcpy(key.ptrw(), script_encryption_key, 32);
	Ref<FileAccessEncrypted> encrypted;
	encrypted.instantiate();
	REQUIRE(encrypted->open_and_parse(backing, key, FileAccessEncrypted::MODE_WRITE_AES256, false) == OK);
	REQUIRE(encrypted->store_buffer(p_plain.ptr(), p_plain.size()));
	encrypted->close();
	const uint64_t size = backing->get_length();
	backing->seek(0);
	const Vector<uint8_t> result = backing->get_buffer(size);
	REQUIRE(result.size() == int64_t(size));
	return result;
}
static Frame frame(int p_version, const Vector<Entry> &p_entries, bool p_bad_last = false, bool p_encrypted = false, bool p_sparse = false, int p_prefix = 0, bool p_relative = true) {
	Frame result;
	Vector<uint8_t> directory;
	Vector<uint8_t> payload;
	const bool salt = p_sparse && p_encrypted && p_version >= 4;
	const uint64_t header_size = (p_version == 2 ? 96 : 104) + (salt ? 32 : 0);
	for (const Entry &entry : p_entries) {
		result.entry_positions.push_back(p_prefix + header_size + 4 + directory.size());
		const CharString path = entry.name.trim_prefix("res://").utf8();
		const int padded = (path.length() + 3) & ~3;
		u32(directory, padded);
		for (int i = 0; i < padded; i++) {
			directory.push_back(i < path.length() ? uint8_t(path[i]) : 0);
		}
		const Vector<uint8_t> bytes = entry.value.to_utf8_buffer();
		u64(directory, p_sparse ? 0 : payload.size());
		u64(directory, bytes.size());
		if (p_version >= 5) {
			u64(directory, bytes.size());
		}
		uint8_t digest[16];
		REQUIRE(CryptoCore::md5(bytes.ptr(), bytes.size(), digest) == OK);
		for (uint8_t byte : digest) {
			directory.push_back(byte);
		}
		u32(directory, entry.flags);
		payload.append_array(bytes);
	}
	if (p_bad_last) {
		u32(directory, UINT32_MAX);
	}
	if (p_encrypted) {
		directory = encrypted_directory(directory);
	}
	REQUIRE(result.bytes.resize(p_prefix) == OK);
	if (p_prefix > 0) {
		result.bytes.fill(0x5a);
	}
	u32(result.bytes, PACK_HEADER_MAGIC);
	u32(result.bytes, p_version);
	u32(result.bytes, 4);
	u32(result.bytes, 0);
	u32(result.bytes, 0);
	u32(result.bytes, (p_relative ? PACK_REL_FILEBASE : 0) | (p_encrypted ? PACK_DIR_ENCRYPTED : 0) | (p_sparse ? PACK_SPARSE_BUNDLE : 0));
	uint64_t file_base = p_sparse ? 0 : header_size + 4 + directory.size();
	if (p_version == 2 && !p_relative) {
		file_base += p_prefix;
	}
	u64(result.bytes, file_base);
	if (p_version >= 3) {
		u64(result.bytes, header_size);
	}
	if (salt) {
		for (int i = 0; i < 32; i++) {
			result.bytes.push_back(uint8_t(i));
		}
	}
	for (int i = 0; i < 16; i++) {
		u32(result.bytes, 0);
	}
	REQUIRE(result.bytes.size() == int64_t(p_prefix + header_size));
	u32(result.bytes, p_entries.size() + (p_bad_last ? 1 : 0));
	result.bytes.append_array(directory);
	result.directory_end = result.bytes.size();
	if (!p_sparse) {
		result.bytes.append_array(payload);
	}
	return result;
}
struct Fixture {
	static inline uint64_t sequence = 0;
	String token = "n036_" + uitos(OS::get_singleton()->get_ticks_usec()) + "_" + uitos(sequence++);
	String existing = "res://" + token + "/original/" + String("stable").repeat(20);
	String added = "res://" + token + "/added/" + String("new").repeat(40);
	String path = TestUtils::get_temp_path(token + "_attempt.pck");
	String baseline = TestUtils::get_temp_path(token + "_baseline.pck");
	HashSet<String> owned;
	PackedSourcePCK source;
	void write(const String &p_path, const Frame &p_frame) {
		Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
		REQUIRE(file.is_valid());
		if (!p_frame.bytes.is_empty()) {
			REQUIRE(file->store_buffer(p_frame.bytes.ptr(), p_frame.bytes.size()));
		}
		file->close();
	}
	bool mount(const Frame &p_frame, bool p_replace = true, int p_offset = 0) {
		write(path, p_frame);
		return source.try_open_pack(path, p_replace, p_offset);
	}
	void remember(const String &p_name) { owned.insert("res://" + p_name.trim_prefix("res://")); }
	void prepare(int p_version) {
		remember(existing);
		remember(added);
		const Frame initial = frame(p_version, { { existing, "Original" } });
		write(baseline, initial);
		REQUIRE(source.try_open_pack(baseline, true, 0));
	}
	String read(const String &p_name) const {
		Ref<FileAccess> file = FileAccess::open(p_name, FileAccess::READ);
		REQUIRE(file.is_valid());
		return file->get_as_utf8_string();
	}
	~Fixture() {
		for (const String &name : owned) {
			PackedData::get_singleton()->remove_path(name);
		}
		for (const String &name : { path, baseline }) {
			if (FileAccess::exists(name)) {
				DirAccess::remove_absolute(name);
			}
		}
	}
};
struct Snapshot {
	HashSet<String> paths = PackedData::get_singleton()->get_file_paths();
	PackedData::PackedFile entry;
	Vector<PackedData::PackedFile> patches;
	explicit Snapshot(const Fixture &p_fixture) {
		entry = Access::entry(p_fixture.existing);
		patches = PackedData::get_singleton()->get_delta_patches(p_fixture.existing);
	}
	void unchanged(const Fixture &p_fixture) const {
		PackedData *data = PackedData::get_singleton();
		const HashSet<String> after = data->get_file_paths();
		CHECK(after.size() == paths.size());
		for (const String &path : paths) {
			CHECK(after.has(path));
		}
		REQUIRE(data->has_path(p_fixture.existing));
		const PackedData::PackedFile current = Access::entry(p_fixture.existing);
		CHECK(current.pack == entry.pack);
		CHECK(current.offset == entry.offset);
		CHECK(current.size == entry.size);
		CHECK(current.stored_size == entry.stored_size);
		CHECK(current.cache_identity == entry.cache_identity);
		CHECK(current.src == entry.src);
		CHECK(current.salt == entry.salt);
		CHECK(memcmp(current.md5, entry.md5, 16) == 0);
		const Vector<PackedData::PackedFile> delta = data->get_delta_patches(p_fixture.existing);
		CHECK(delta.size() == patches.size());
		for (int64_t i = 0; i < MIN(delta.size(), patches.size()); i++) {
			CHECK(delta[i].cache_identity == patches[i].cache_identity);
		}
		CHECK(p_fixture.read(p_fixture.existing) == "Original");
		CHECK_FALSE(data->has_path(p_fixture.added));
		CHECK_FALSE(data->has_directory("res://" + p_fixture.token + "/added"));
	}
};

TEST_CASE("[N036][PCKMountAtomicity] Versions two through five preserve padded Unicode paths offsets and relative bases") {
	for (int version : { 2, 3, 4, 5 }) {
		for (int prefix : { 0, 17 }) {
			for (bool relative : { false, true }) {
				Fixture fixture;
				const String name = "res://" + fixture.token + "/unicode_Ã©.bin";
				fixture.remember(name);
				REQUIRE(fixture.mount(frame(version, { { name, "Original" } }, false, false, false, prefix, relative), true, prefix));
				CHECK(fixture.read(name) == "Original");
			}
		}
	}
}
TEST_CASE("[N036][PCKMountAtomicity] Truncated header path or metadata never publishes entries") {
	for (int version : { 2, 3, 4, 5 }) {
		Fixture fixture;
		fixture.prepare(version);
		const Snapshot before(fixture);
		const Frame complete = frame(version, { { fixture.added, "Changed" } });
		const uint64_t header = version == 2 ? 96 : 104;
		for (uint64_t length : { uint64_t(0), uint64_t(3), uint64_t(4), uint64_t(7), uint64_t(23), uint64_t(31),
					 header - 1, header, header + 3, header + 7, complete.directory_end - 1 }) {
			Frame truncated = complete;
			REQUIRE(truncated.bytes.resize(length) == OK);
			QuietErrors quiet;
			CHECK_FALSE(fixture.mount(truncated));
			before.unchanged(fixture);
		}
	}
}
TEST_CASE("[N036][PCKMountAtomicity] A malformed later entry exposes no add replace removal or delta prefix") {
	for (int version : { 2, 3, 4, 5 }) {
		for (int operation = 0; operation < 4; operation++) {
			Fixture fixture;
			fixture.prepare(version);
			const Snapshot before(fixture);
			Entry first;
			first.name = operation == 0 ? fixture.added : fixture.existing;
			first.value = operation == 2 ? "" : "Changed";
			first.flags = operation == 2 ? PACK_FILE_REMOVAL : operation == 3 ? PACK_FILE_DELTA
																			  : 0;
			QuietErrors quiet;
			CHECK_FALSE(fixture.mount(frame(version, { first }, true)));
			before.unchanged(fixture);
		}
	}
}
TEST_CASE("[N036][PCKMountAtomicity] Encrypted directories validate completely before publication") {
	for (int version : { 2, 3, 4, 5 }) {
		Fixture fixture;
		fixture.prepare(version);
		const Snapshot before(fixture);
		QuietErrors quiet;
		CHECK_FALSE(fixture.mount(frame(version, { { fixture.existing, "", PACK_FILE_REMOVAL } }, true, true)));
		before.unchanged(fixture);
		REQUIRE(fixture.mount(frame(version, { { fixture.existing, "Changed" } }, false, true)));
		CHECK(fixture.read(fixture.existing) == "Changed");
	}
}
TEST_CASE("[N036][PCKMountAtomicity] Sparse metadata retains external zero offsets and salt conversion") {
	for (int version : { 4, 5 }) {
		for (bool encrypted : { false, true }) {
			Fixture fixture;
			fixture.remember(fixture.existing);
			QuietErrors quiet;
			REQUIRE(fixture.mount(frame(version, { { fixture.existing, "External" } }, false, encrypted, true)));
			const PackedData::PackedFile entry = Access::entry(fixture.existing);
			CHECK(entry.bundle);
			CHECK(entry.offset == 0);
			CHECK(entry.size == 8);
			if (encrypted) {
				CHECK(entry.salt.length() == 32);
				CHECK(entry.salt[0] == 0xfffd);
			}
		}
	}
}
TEST_CASE("[N036][PCKMountAtomicity] Appended footer discovery rejects underflow and excessive offsets") {
	Fixture fixture;
	fixture.remember(fixture.existing);
	const Frame pack = frame(5, { { fixture.existing, "Original" } });
	Frame appended;
	REQUIRE(appended.bytes.resize(37) == OK);
	appended.bytes.fill(0x5a);
	appended.bytes.append_array(pack.bytes);
	u64(appended.bytes, pack.bytes.size());
	u32(appended.bytes, PACK_HEADER_MAGIC);
	REQUIRE(fixture.mount(appended));
	CHECK(fixture.read(fixture.existing) == "Original");
	const Snapshot before(fixture);
	Frame invalid = appended;
	encode_uint64(UINT64_MAX, invalid.bytes.ptrw() + invalid.bytes.size() - 12);
	QuietErrors quiet;
	CHECK_FALSE(fixture.mount(invalid));
	before.unchanged(fixture);
	CHECK_FALSE(fixture.source.try_open_pack(fixture.path, true, UINT64_MAX));
}
TEST_CASE("[N036][PCKMountAtomicity] A valid directory commits removal and addition in recorded order") {
	Fixture fixture;
	fixture.prepare(5);
	REQUIRE(fixture.mount(frame(5, { { fixture.existing, "", PACK_FILE_REMOVAL }, { fixture.existing, "Changed" } }), false));
	CHECK(fixture.read(fixture.existing) == "Changed");
}

class FaultReader : public FileAccessMemory {
public:
	uint64_t fail_at = UINT64_MAX;
	bool hard = false;
	mutable bool failed = false;
	uint64_t get_buffer(uint8_t *p_dst, uint64_t p_length) const override {
		if (get_position() >= fail_at && p_length > 0) {
			failed = true;
			return FileAccessMemory::get_buffer(p_dst, hard ? p_length : p_length - 1);
		}
		return FileAccessMemory::get_buffer(p_dst, p_length);
	}
	Error get_error() const override {
		return failed && hard ? ERR_FILE_CANT_READ : FileAccessMemory::get_error();
	}
};
class MemorySource : public PackSource {
public:
	Vector<uint8_t> bytes;
	uint64_t fail_at = 0;
	bool hard = false;
	bool try_open_pack(const String &, bool, uint64_t, const Vector<uint8_t> &) override { return false; }
	Ref<FileAccess> get_file(const String &, PackedData::PackedFile *, const Vector<uint8_t> &, const Vector<PackedData::PackedFile> *) override {
		Ref<FaultReader> reader = memnew(FaultReader);
		if (reader->open_custom(bytes.ptr(), bytes.size()) != OK) {
			return Ref<FileAccess>();
		}
		reader->fail_at = fail_at;
		reader->hard = hard;
		return reader;
	}
};
TEST_CASE("[N036][PCKMountAtomicity] Short reads and hard errors after a staged entry leave mounts unchanged") {
	for (bool hard : { false, true }) {
		Fixture fixture;
		fixture.prepare(5);
		const Frame complete = frame(5, { { fixture.added, "Changed" }, { fixture.token + "/second", "Second" } });
		MemorySource memory;
		memory.bytes = complete.bytes;
		memory.fail_at = complete.entry_positions[1];
		memory.hard = hard;
		const String container = "res://" + fixture.token + "/container.pck";
		fixture.remember(container);
		uint8_t digest[16] = {};
		PackedData::get_singleton()->add_path("memory", container, 1, memory.bytes.size(), memory.bytes.size(), digest, &memory, true);
		const Snapshot before(fixture);
		QuietErrors quiet;
		CHECK_FALSE(fixture.source.try_open_pack(container, true, 0));
		before.unchanged(fixture);
		PackedData::get_singleton()->remove_path(container);
	}
}
#ifdef B01_ALLOCATION_TESTS
TEST_CASE("[N036][PCKMountAtomicityAllocation] Pending entry allocation failure publishes nothing and permits retry") {
	Fixture fixture;
	fixture.prepare(5);
	const Snapshot before(fixture);
	Vector<Entry> entries;
	for (int i = 0; i < 37; i++) {
		const String name = "res://" + fixture.token + "/item_" + itos(i);
		fixture.remember(name);
		entries.push_back({ name, "Value" });
	}
	const Frame complete = frame(5, entries);
	fixture.write(fixture.path, complete);
	const size_t allocation = TestAllocation::allocation_size([]() {
		Vector<PCKDirectory::Entry> pending;
		REQUIRE(pending.resize(37) == OK);
	});
	QuietErrors quiet;
	{
		TestAllocation::Failure failure(allocation);
		CHECK_FALSE(fixture.source.try_open_pack(fixture.path, true, 0));
		CHECK(TestAllocation::failures() == 1);
	}
	before.unchanged(fixture);
	REQUIRE(fixture.source.try_open_pack(fixture.path, true, 0));
	for (const Entry &entry : entries) {
		CHECK(fixture.read(entry.name) == "Value");
	}
}
TEST_CASE("[N036][PCKMountAtomicityAllocation] Path byte and decoded string allocation failures preserve metadata") {
	for (bool decoded : { false, true }) {
		Fixture fixture;
		fixture.prepare(5);
		const Snapshot before(fixture);
		const String name = "res://" + fixture.token + "/" + String("n").repeat(1000);
		fixture.remember(name);
		const Frame complete = frame(5, { { name, "Value" } });
		fixture.write(fixture.path, complete);
		const CharString path = name.trim_prefix("res://").utf8();
		const int padded = (path.length() + 3) & ~3;
		const size_t allocation = TestAllocation::allocation_size([&]() {
			if (decoded) {
				String string;
				REQUIRE(string.reserve(uint64_t(padded) + 1) == OK);
			} else {
				CharString bytes;
				REQUIRE(bytes.resize_uninitialized(uint64_t(padded) + 1) == OK);
			}
		});
		QuietErrors quiet;
		{
			TestAllocation::Failure failure(allocation);
			CHECK_FALSE(fixture.source.try_open_pack(fixture.path, true, 0));
			CHECK(TestAllocation::failures() == 1);
		}
		before.unchanged(fixture);
		REQUIRE(fixture.source.try_open_pack(fixture.path, true, 0));
		CHECK(fixture.read(name) == "Value");
	}
}
#endif
} // namespace TestPCKMountAtomicity
