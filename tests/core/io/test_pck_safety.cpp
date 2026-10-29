/**************************************************************************/
/*  test_pck_safety.cpp                                                   */
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

TEST_FORCE_LINK(test_pck_safety)

#include "core/io/dir_access.h"
#include "core/io/delta_encoding.h"
#include "core/io/file_access_patched.h"
#include "core/io/file_access_encrypted.h"
#include "tests/core/io/encrypted_test_helpers.h"
#include "core/io/file_access_memory.h"
#include "core/io/file_access_pack.h"
#include "core/io/pck_lzma.h"
#include "tests/core/io/pck_lzma2_fixtures.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "tests/test_utils.h"

#include "modules/modules_enabled.gen.h"
#ifdef MODULE_JPG_ENABLED
#include "tests/core/io/jpeg_test_helpers.h"
#endif
#ifdef MODULE_WEBP_ENABLED
#include "modules/webp/texture_loader_webp.h"
#include "modules/webp/webp_common.h"
#include "tests/core/io/webp_test_helpers.h"
#endif

#include <cstring>
#include <atomic>

namespace TestPCKSafety {

struct Access {
	static void restore(PackedData *p_previous) { PackedData::singleton = p_previous; }
	static void backing(FileAccessPack &p_pack, const Ref<FileAccess> &p_file) { p_pack.f = p_file; p_pack.off = 0; }
	static uint64_t backing_position(FileAccessPack &p_pack) { return p_pack.f.is_valid() ? p_pack.f->get_position() : 0; }
};

struct ErrorsOff {
	bool previous = CoreGlobals::print_error_enabled;
	ErrorsOff() { CoreGlobals::print_error_enabled = false; }
	~ErrorsOff() { CoreGlobals::print_error_enabled = previous; }
};

static Vector<uint8_t> bytes(int p_size, bool p_random = false) {
	Vector<uint8_t> result;
	result.resize(p_size);
	uint32_t state = 0x612fa392;
	for (int i = 0; i < p_size; i++) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		result.write[i] = p_random ? uint8_t(state) : uint8_t(i % 13);
	}
	return result;
}

struct Fixture {
	String directory = TestUtils::get_temp_path("b01_" + uitos(OS::get_singleton()->get_ticks_usec()));
	String filename = directory.path_join("entry.bin");
	String logical = directory.path_join("logical.bin");
	PackedSourcePCK source;
	PackedData::PackedFile info = {};
	bool registered = false;

	Fixture() {
		REQUIRE(DirAccess::make_dir_absolute(directory) == OK);
		info.pack = filename;
		info.offset = 1;
		info.src = &source;
	}
	void write(const Vector<uint8_t> &p_stored, uint64_t p_size, bool p_compressed = false, bool p_encrypted = false) {
		Ref<FileAccess> file = FileAccess::open(filename, FileAccess::WRITE);
		REQUIRE(file.is_valid());
		file->store_8(0x57);
		if (p_encrypted) {
			Vector<uint8_t> key;
			key.resize(32);
			memcpy(key.ptrw(), script_encryption_key, 32);
			Ref<FileAccessEncrypted> encrypted;
			encrypted.instantiate();
			REQUIRE(encrypted->open_and_parse(file, key, FileAccessEncrypted::MODE_WRITE_AES256, false) == OK);
			REQUIRE(encrypted->store_buffer(p_stored.ptr(), p_stored.size()));
			encrypted->close();
		} else {
			REQUIRE(file->store_buffer(p_stored.ptr(), p_stored.size()));
		}
		info.size = p_size;
		info.stored_size = file->get_position() - 1;
		info.lzma2 = p_compressed;
		info.encrypted = p_encrypted;
	}
	void mount() {
		REQUIRE(PackedData::get_singleton() != nullptr);
		PackedData::get_singleton()->add_path(info.pack, logical, info.offset, info.size, info.stored_size, info.md5, &source, true, info.encrypted, info.bundle, false, info.lzma2);
		registered = true;
	}
	~Fixture() {
		if (registered) {
			PackedData::get_singleton()->remove_path(logical);
		}
		if (FileAccess::exists(filename)) { DirAccess::remove_absolute(filename); }
		if (FileAccess::exists(logical)) { DirAccess::remove_absolute(logical); }
		DirAccess::remove_absolute(directory);
	}
};

static Vector<uint8_t> compressed(const Vector<uint8_t> &p_input) {
	// Decoder tests use recorded streams, independently validated with liblzma.
	// Encoder instrumentation is exercised separately by PCKEncoderDiagnostic.
	if (p_input.size() == 200003) {
		REQUIRE(p_input == bytes(200003, true));
		Vector<uint8_t> output;
		output.push_back(16); // LZMA2 property for a 1 MiB dictionary.
		int64_t offset = 0;
		while (offset < p_input.size()) {
			const int64_t chunk_size = MIN(int64_t(65536), p_input.size() - offset);
			output.push_back(offset == 0 ? 1 : 2); // Uncompressed chunk, reset dictionary on first.
			output.push_back(uint8_t((chunk_size - 1) >> 8));
			output.push_back(uint8_t(chunk_size - 1));
			output.append_array(p_input.slice(offset, offset + chunk_size));
			offset += chunk_size;
		}
		output.push_back(0); // End marker.
		return output;
	}
	const uint8_t *data = nullptr;
	int64_t size = 0;
	switch (p_input.size()) {
		case 1031: data = TestPCKSafetyData::data_1031; size = sizeof(TestPCKSafetyData::data_1031); break;
		case 2051: data = TestPCKSafetyData::data_2051; size = sizeof(TestPCKSafetyData::data_2051); break;
		case 3119: data = TestPCKSafetyData::data_3119; size = sizeof(TestPCKSafetyData::data_3119); break;
		case 4099: data = TestPCKSafetyData::data_4099; size = sizeof(TestPCKSafetyData::data_4099); break;
		case 5003: data = TestPCKSafetyData::data_5003; size = sizeof(TestPCKSafetyData::data_5003); break;
		case 100003: data = TestPCKSafetyData::data_100003; size = sizeof(TestPCKSafetyData::data_100003); break;
		default: FAIL("Missing recorded LZMA2 fixture"); return Vector<uint8_t>();
	}
	REQUIRE(p_input == bytes(int(p_input.size())));
	Vector<uint8_t> output;
	REQUIRE(output.resize(size) == OK);
	memcpy(output.ptrw(), data, size);
	return output;
}



struct ScopedPackedData {
	PackedData *previous = PackedData::get_singleton();
	PackedData data;
	~ScopedPackedData() { Access::restore(previous); }
};
struct MountedPath {
	String path;
	explicit MountedPath(const String &p_path) : path(p_path) {}
	~MountedPath() { PackedData::get_singleton()->remove_path(path); }
};
static Vector<uint8_t> literal_lzma2(const Vector<uint8_t> &p_data) {
	REQUIRE(p_data.size() > 0);
	REQUIRE(p_data.size() <= 65536);
	Vector<uint8_t> stored;
	REQUIRE(stored.resize(p_data.size() + 5) == OK);
	stored.write[0] = 0; // 4 KiB dictionary; these controls fit in one chunk.
	stored.write[1] = 1; // Uncompressed LZMA2 chunk with dictionary reset.
	stored.write[2] = (p_data.size() - 1) >> 8;
	stored.write[3] = (p_data.size() - 1) & 255;
	memcpy(stored.ptrw() + 4, p_data.ptr(), p_data.size());
	stored.write[stored.size() - 1] = 0;
	return stored;
}
static void map_entry(const String &p_path, const PackedData::PackedFile &p_file) {
	PackedData::get_singleton()->add_path(p_file.pack, p_path, p_file.offset, p_file.size, p_file.stored_size,
			p_file.md5, p_file.src, true, p_file.encrypted, p_file.bundle, p_file.delta, p_file.lzma2, p_file.salt);
}
TEST_CASE("[F14][PCKCache] Replacement, removal and clear preserve current and already-open content") {
	ScopedPackedData isolated;
	Fixture fixture;
	const Vector<uint8_t> first = bytes(1031);
	Vector<uint8_t> second = first;
	second.write[17] ^= 0x57;
	fixture.write(literal_lzma2(first), first.size(), true);
	fixture.mount();
	Ref<FileAccess> held = FileAccess::open(fixture.logical, FileAccess::READ);
	REQUIRE(held.is_valid());
	CHECK(held->get_buffer(first.size()) == first);
	const uint64_t old_extent = fixture.info.stored_size;
	fixture.write(literal_lzma2(second), second.size(), true);
	REQUIRE(fixture.info.stored_size == old_extent);
	fixture.mount();
	Ref<FileAccess> replaced = FileAccess::open(fixture.logical, FileAccess::READ);
	REQUIRE(replaced.is_valid());
	CHECK(replaced->get_buffer(second.size()) == second);
	held->seek(0);
	CHECK(held->get_buffer(first.size()) == first);
	PackedData::get_singleton()->remove_path(fixture.logical);
	fixture.write(literal_lzma2(first), first.size(), true);
	fixture.mount();
	REQUIRE(FileAccess::get_file_as_bytes(fixture.logical) == first);
	PackedData::get_singleton()->clear();
	fixture.write(literal_lzma2(second), second.size(), true);
	fixture.mount();
	REQUIRE(FileAccess::get_file_as_bytes(fixture.logical) == second);
	replaced->seek(0);
	CHECK(replaced->get_buffer(second.size()) == second);
	held->seek(0);
	CHECK(held->get_buffer(first.size()) == first);
}
TEST_CASE("[F14][PCKCache] Equal-size sparse entries in one owner retain separate identities") {
	ScopedPackedData isolated;
	Fixture first_file;
	Fixture second_file;
	const String stem = "f14_sparse_" + uitos(OS::get_singleton()->get_ticks_usec());
	first_file.filename = first_file.directory.path_join(stem + "_a.bin");
	second_file.filename = first_file.directory.path_join(stem + "_b.bin");
	second_file.info.pack = first_file.info.pack;
	const Vector<uint8_t> first = bytes(2051);
	Vector<uint8_t> second = first;
	second.write[19] ^= 0xa7;
	const Vector<uint8_t> stored_a = literal_lzma2(first);
	const Vector<uint8_t> stored_b = literal_lzma2(second);
	REQUIRE(stored_a.size() == stored_b.size());
	for (Fixture *fixture : { &first_file, &second_file }) {
		Ref<FileAccess> output = FileAccess::open(fixture->filename, FileAccess::WRITE);
		REQUIRE(output.is_valid());
		const Vector<uint8_t> &stored = fixture == &first_file ? stored_a : stored_b;
		REQUIRE(output->store_buffer(stored.ptr(), stored.size()));
		output->close();
		fixture->info.bundle = true;
		fixture->info.lzma2 = true;
		fixture->info.size = first.size();
		fixture->info.stored_size = stored.size();
	}
#ifdef TOOLS_ENABLED
	const String path_a = "res://" + first_file.filename.get_file();
	const String path_b = "res://" + second_file.filename.get_file();
#else
	const String path_a = first_file.filename;
	const String path_b = second_file.filename;
#endif
	MountedPath mount_a(path_a);
	MountedPath mount_b(path_b);
	map_entry(path_a, first_file.info);
	map_entry(path_b, second_file.info);
	for (int repeat = 0; repeat < 2; repeat++) {
		CHECK(FileAccess::get_file_as_bytes(path_a) == first);
		CHECK(FileAccess::get_file_as_bytes(path_b) == second);
	}
}
struct ConcurrentCacheReads {
	String path;
	Vector<uint8_t> first;
	Vector<uint8_t> second;
	std::atomic<int> failures{ 0 };
	static void run(void *p_userdata) {
		ConcurrentCacheReads *work = static_cast<ConcurrentCacheReads *>(p_userdata);
		for (int i = 0; i < 64; i++) {
			Error error = FAILED;
			Ref<FileAccess> file = FileAccess::open(work->path, FileAccess::READ, &error);
			if (error != OK || file.is_null()) {
				work->failures.fetch_add(1, std::memory_order_relaxed);
				continue;
			}
			const Vector<uint8_t> output = file->get_buffer(file->get_length());
			if ((output != work->first && output != work->second) || file->get_error() != OK) {
				work->failures.fetch_add(1, std::memory_order_relaxed);
			}
		}
	}
};
TEST_CASE("[F14][PCKCache] Opens remain coherent while accepted mappings switch between immutable backing files") {
	ScopedPackedData isolated;
	Fixture first_file;
	Fixture second_file;
	ConcurrentCacheReads work;
	work.path = first_file.logical;
	work.first = bytes(2051);
	work.second = work.first;
	work.second.write[19] ^= 0x57;
	first_file.write(literal_lzma2(work.first), work.first.size(), true);
	second_file.write(literal_lzma2(work.second), work.second.size(), true);
	MountedPath mounted(work.path);
	map_entry(work.path, first_file.info);
#ifdef THREADS_ENABLED
	Thread readers[3];
	for (Thread &reader : readers) {
		reader.start(&ConcurrentCacheReads::run, &work);
		CHECK(reader.is_started());
	}
#endif
	for (int i = 0; i < 64; i++) {
		map_entry(work.path, i % 2 ? first_file.info : second_file.info);
#ifndef THREADS_ENABLED
		Error error = FAILED;
		Ref<FileAccess> file = FileAccess::open(work.path, FileAccess::READ, &error);
		REQUIRE(error == OK);
		REQUIRE(file.is_valid());
		CHECK(file->get_buffer(file->get_length()) == (i % 2 ? work.first : work.second));
#endif
	}
#ifdef THREADS_ENABLED
	for (Thread &reader : readers) {
		if (reader.is_started()) { reader.wait_to_finish(); }
	}
#else
	ConcurrentCacheReads::run(&work);
#endif
	CHECK(work.failures.load(std::memory_order_relaxed) == 0);
}

static Vector<uint8_t> encoded_delta(const Vector<uint8_t> &p_old, const Vector<uint8_t> &p_new) {
	Vector<uint8_t> delta;
	REQUIRE(DeltaEncoding::encode_delta(p_old, p_new, delta, 3) == OK);
	REQUIRE(delta.size() > 5);
	return delta;
}
TEST_CASE("[N002][PCKDelta] Compression and encryption flags decode logical patch bytes") {
	for (bool compressed_entry : { false, true }) {
		for (bool encrypted_entry : { false, true }) {
			ScopedPackedData isolated;
			Fixture base;
			Fixture patch;
			MountedPath mounted(base.logical);
			const Vector<uint8_t> original = bytes(1031);
			Vector<uint8_t> expected = original;
			expected.write[17] ^= 0xa7;
			base.write(original, original.size());
			base.mount();
			const Vector<uint8_t> delta = encoded_delta(original, expected);
			patch.write(compressed_entry ? literal_lzma2(delta) : delta, delta.size(), compressed_entry, encrypted_entry);
			patch.info.delta = true;
			map_entry(base.logical, patch.info);
			Error error = FAILED;
			Ref<FileAccess> reader = FileAccess::open(base.logical, FileAccess::READ, &error);
			REQUIRE(error == OK);
			REQUIRE(reader.is_valid());
			CHECK(reader->get_length() == uint64_t(expected.size()));
			CHECK(reader->get_error() == OK);
			CHECK(reader->get_buffer(expected.size()) == expected);
			reader->seek(17);
			CHECK(reader->get_buffer(1) == expected.slice(17, 18));
		}
	}
#ifdef TOOLS_ENABLED
	constexpr uint8_t empty_key[32] = {};
	if (memcmp(script_encryption_key, empty_key, sizeof(empty_key)) == 0) {
		ScopedPackedData isolated;
		Fixture base;
		Fixture patch;
		MountedPath mounted(base.logical);
		const Vector<uint8_t> original = bytes(1031);
		Vector<uint8_t> expected = original;
		expected.write[19] ^= 0x57;
		base.write(original, original.size());
		base.mount();
		const Vector<uint8_t> delta = encoded_delta(original, expected);
		const Vector<uint8_t> stored = literal_lzma2(delta);
		const Vector<uint8_t> custom_key = TestEncryptedSafety::key();
		Ref<FileAccess> output = FileAccess::open(patch.filename, FileAccess::WRITE);
		REQUIRE(output.is_valid());
		REQUIRE(output->store_8(0xa7));
		FileAccessEncrypted writer;
		REQUIRE(writer.open_and_parse(output, custom_key, FileAccessEncrypted::MODE_WRITE_AES256, false) == OK);
		REQUIRE(writer.store_buffer(stored.ptr(), stored.size()));
		writer.close();
		patch.info.stored_size = output->get_position() - 1;
		output->close();
		patch.info.size = delta.size();
		patch.info.encrypted = true;
		patch.info.lzma2 = true;
		patch.info.delta = true;
		map_entry(base.logical, patch.info);
		Error error = FAILED;
		Ref<FileAccess> reader = PackedData::get_singleton()->try_open_path(base.logical, custom_key, &error);
		REQUIRE(error == OK);
		REQUIRE(reader.is_valid());
		CHECK(reader->get_length() == uint64_t(expected.size()));
		CHECK(reader->get_buffer(expected.size()) == expected);
	}
#endif
}
TEST_CASE("[N002][PCKDelta] Ordered patch chains preserve nonempty and empty final content") {
	for (bool empty_final : { false, true }) {
		ScopedPackedData isolated;
		Fixture base;
		Fixture first_patch;
		Fixture second_patch;
		MountedPath mounted(base.logical);
		const Vector<uint8_t> original = bytes(2051);
		const Vector<uint8_t> middle = bytes(3119);
		const Vector<uint8_t> expected = empty_final ? Vector<uint8_t>() : bytes(1031);
		base.write(original, original.size());
		base.mount();
		const Vector<uint8_t> first = encoded_delta(original, middle);
		const Vector<uint8_t> second = encoded_delta(middle, expected);
		first_patch.write(literal_lzma2(first), first.size(), true);
		first_patch.info.delta = true;
		second_patch.write(literal_lzma2(second), second.size(), true, true);
		second_patch.info.delta = true;
		map_entry(base.logical, first_patch.info);
		map_entry(base.logical, second_patch.info);
		Ref<FileAccess> reader = FileAccess::open(base.logical, FileAccess::READ);
		REQUIRE(reader.is_valid());
		CHECK(reader->get_length() == uint64_t(expected.size()));
		CHECK(reader->is_open());
		CHECK(reader->get_buffer(expected.size() + 1) == expected);
		CHECK(reader->get_error() == ERR_FILE_EOF);
		reader->seek(0);
		CHECK(reader->get_buffer(expected.size() + 1) == expected);
	}
}
TEST_CASE("[N002][PCKDelta] Incomplete staging and unsupported bundle patches retain a failure") {
	ErrorsOff quiet;
	for (bool bundle : { false, true }) {
		ScopedPackedData isolated;
		Fixture base;
		Fixture patch;
		MountedPath mounted(base.logical);
		const Vector<uint8_t> original = bytes(1031);
		const Vector<uint8_t> expected = bytes(2051);
		base.write(original, original.size());
		base.mount();
		const Vector<uint8_t> delta = encoded_delta(original, expected);
		patch.write(delta, delta.size());
		patch.info.delta = true;
		patch.info.bundle = bundle;
		if (!bundle) { patch.info.size += 5; }
		map_entry(base.logical, patch.info);
		Ref<FileAccess> reader = FileAccess::open(base.logical, FileAccess::READ);
		REQUIRE(reader.is_valid());
		CHECK(reader->get_length() == 0);
		const Error failure = reader->get_error();
		CHECK(failure == (bundle ? FAILED : ERR_FILE_CORRUPT));
		uint8_t output = 0xa7;
		CHECK(reader->get_buffer(&output, 1) == 0);
		CHECK(output == 0xa7);
		reader->seek(0);
		CHECK(reader->get_length() == 0);
		CHECK(reader->get_error() == failure);
	}
}

TEST_CASE("[N009][PCKKey] Compiled-key priority and eligible override policy always produce full keys") {
	uint8_t compiled[32];
	constexpr uint8_t empty[32] = {};
	for (int i = 0; i < 32; i++) { compiled[i] = i; }
	Vector<uint8_t> provided = bytes(32);
	Vector<uint8_t> short_key = bytes(3);
	Vector<uint8_t> selected;
	for (bool allow : { false, true }) {
		REQUIRE(PCKEncryption::select_key(Vector<uint8_t>(), compiled, allow, selected) == OK);
		REQUIRE(selected.size() == 32);
		CHECK(memcmp(selected.ptr(), compiled, 32) == 0);
		REQUIRE(PCKEncryption::select_key(provided, compiled, allow, selected) == OK);
		REQUIRE(selected.size() == 32);
		CHECK(memcmp(selected.ptr(), compiled, 32) == 0);
		REQUIRE(PCKEncryption::select_key(Vector<uint8_t>(), empty, allow, selected) == OK);
		REQUIRE(selected.size() == 32);
		CHECK(memcmp(selected.ptr(), empty, 32) == 0);
		REQUIRE(PCKEncryption::select_key(provided, empty, allow, selected) == OK);
		if (allow) {
			CHECK(selected == provided);
		} else {
			CHECK(memcmp(selected.ptr(), empty, 32) == 0);
		}
	}
	REQUIRE(PCKEncryption::select_key(short_key, compiled, false, selected) == OK);
	CHECK(memcmp(selected.ptr(), compiled, 32) == 0);
}
TEST_CASE("[N009][PCKKey] A valid compiled key remains usable when a different override is supplied") {
	uint8_t compiled[32];
	for (int i = 0; i < 32; i++) { compiled[i] = i; }
	const Vector<uint8_t> encrypted = TestEncryptedSafety::stream(17, false);
	Vector<uint8_t> selected;
	REQUIRE(PCKEncryption::select_key(bytes(32), compiled, true, selected) == OK);
	Ref<FileAccessMemory> source;
	source.instantiate();
	REQUIRE(source->open_custom(encrypted.ptr(), encrypted.size()) == OK);
	FileAccessEncrypted reader;
	REQUIRE(reader.open_and_parse(source, selected, FileAccessEncrypted::MODE_READ, false) == OK);
	Vector<uint8_t> expected;
	REQUIRE(expected.resize(17) == OK);
	for (int i = 0; i < 17; i++) { expected.write[i] = i; }
	CHECK(reader.FileAccess::get_buffer(expected.size()) == expected);
}
TEST_CASE("[N009][PCKKey] Eligible invalid overrides clear output and valid aliasing remains safe") {
	uint8_t compiled[32];
	constexpr uint8_t empty[32] = {};
	for (int i = 0; i < 32; i++) { compiled[i] = i; }
	const Vector<uint8_t> provided = bytes(32);
	Vector<uint8_t> selected = provided;
	CHECK(PCKEncryption::select_key(bytes(3), compiled, true, selected) == ERR_INVALID_PARAMETER);
	CHECK(selected.is_empty());
	selected = provided;
	CHECK(PCKEncryption::select_key(bytes(3), empty, true, selected) == ERR_INVALID_PARAMETER);
	CHECK(selected.is_empty());
	selected = provided;
	CHECK(PCKEncryption::select_key(selected, empty, true, selected) == OK);
	CHECK(selected == provided);
}

struct DeltaSnapshotFixture {
	ScopedPackedData isolated;
	Fixture base_a;
	Fixture base_b;
	Fixture patch_a;
	Fixture patch_b;
	MountedPath mounted;
	Vector<uint8_t> expected_a;
	Vector<uint8_t> expected_b;
	DeltaSnapshotFixture() : mounted(base_a.logical) {
		const Vector<uint8_t> original_a = bytes(1031);
		const Vector<uint8_t> original_b = bytes(2051);
		expected_a = bytes(3119);
		expected_b = bytes(4099);
		base_a.write(original_a, original_a.size());
		base_b.write(original_b, original_b.size());
		const Vector<uint8_t> first = encoded_delta(original_a, expected_a);
		const Vector<uint8_t> second = encoded_delta(original_b, expected_b);
		patch_a.write(literal_lzma2(first), first.size(), true);
		patch_b.write(literal_lzma2(second), second.size(), true);
		patch_a.info.delta = true;
		patch_b.info.delta = true;
	}
	void first() {
		map_entry(base_a.logical, base_a.info);
		map_entry(base_a.logical, patch_a.info);
	}
	void second() {
		map_entry(base_a.logical, base_b.info);
		map_entry(base_a.logical, patch_b.info);
	}
};
class RemountingPCKSource : public PackedSourcePCK {
public:
	DeltaSnapshotFixture *fixture = nullptr;
	bool remounted = false;
	Ref<FileAccess> get_file(const String &p_path, PackedData::PackedFile *p_file, const Vector<uint8_t> &p_key, const Vector<PackedData::PackedFile> *p_delta_snapshot) override {
		if (!remounted) {
			remounted = true;
			fixture->second();
		}
		return PackedSourcePCK::get_file(p_path, p_file, p_key, p_delta_snapshot);
	}
};
TEST_CASE("[N010][PCKSnapshot] Open readers retain coherent chains across deferred reads and provider remounts") {
	for (bool during_provider : { false, true }) {
		DeltaSnapshotFixture fixture;
		RemountingPCKSource source;
		source.fixture = &fixture;
		if (during_provider) { fixture.base_a.info.src = &source; }
		fixture.first();
		Ref<FileAccess> old_reader = FileAccess::open(fixture.base_a.logical, FileAccess::READ);
		REQUIRE(old_reader.is_valid());
		if (during_provider) {
			CHECK(source.remounted);
		} else {
			fixture.second();
		}
		// Application is deliberately deferred until after the valid remount.
		CHECK(old_reader->get_length() == uint64_t(fixture.expected_a.size()));
		CHECK(old_reader->get_buffer(fixture.expected_a.size()) == fixture.expected_a);
		Ref<FileAccess> new_reader = FileAccess::open(fixture.base_a.logical, FileAccess::READ);
		REQUIRE(new_reader.is_valid());
		CHECK(new_reader->get_length() == uint64_t(fixture.expected_b.size()));
		CHECK(new_reader->get_buffer(fixture.expected_b.size()) == fixture.expected_b);
	}
}
struct UIDMapping {
	ResourceUID::ID id = ResourceUID::INVALID_ID;
	explicit UIDMapping(const String &p_path) {
		id = ResourceUID::get_singleton()->create_id_for_path(p_path);
		REQUIRE(id != ResourceUID::INVALID_ID);
		ResourceUID::get_singleton()->add_id(id, p_path);
		REQUIRE(ResourceUID::get_singleton()->has_id(id));
	}
	~UIDMapping() {
		if (ResourceUID::get_singleton()->has_id(id)) { ResourceUID::get_singleton()->remove_id(id); }
	}
};
TEST_CASE("[N010][PCKSnapshot] UID aliases select the matching chain and retained readers survive alias removal") {
	DeltaSnapshotFixture fixture;
	fixture.first();
	UIDMapping mapping(fixture.base_a.logical);
	const String uid = ResourceUID::get_singleton()->id_to_text(mapping.id);
	CHECK(PackedData::get_singleton()->has_delta_patches(uid));
	CHECK(PackedData::get_singleton()->get_delta_patches(uid).size() == 1);
	Error error = FAILED;
	Ref<FileAccess> old_reader = PackedData::get_singleton()->try_open_path(uid, Vector<uint8_t>(), &error);
	REQUIRE(error == OK);
	REQUIRE(old_reader.is_valid());
	fixture.second();
	ResourceUID::get_singleton()->remove_id(mapping.id);
	CHECK(old_reader->get_length() == uint64_t(fixture.expected_a.size()));
	CHECK(old_reader->get_buffer(fixture.expected_a.size()) == fixture.expected_a);
	Ref<FileAccess> new_reader = FileAccess::open(fixture.base_a.logical, FileAccess::READ);
	REQUIRE(new_reader.is_valid());
	CHECK(new_reader->get_buffer(new_reader->get_length()) == fixture.expected_b);
}
TEST_CASE("[F08][PCK] Ordinary encryption and LZMA2 combinations preserve logical entry bytes") {
	for (int size : { 1031, 2051, 3119, 4099, 5003 }) {
		const Vector<uint8_t> payload = bytes(size);
		for (bool compressed_entry : { false, true }) {
			const Vector<uint8_t> stored = compressed_entry ? compressed(payload) : payload;
			for (bool encrypted_entry : { false, true }) {
				Fixture fixture;
				fixture.write(stored, payload.size(), compressed_entry, encrypted_entry);
				for (int repeat = 0; repeat < 2; repeat++) {
					FileAccessPack reader(fixture.logical, fixture.info);
					REQUIRE(reader.is_open());
					CHECK(reader.get_error() == OK);
					CHECK(reader.get_length() == uint64_t(size));
					CHECK(reader.FileAccess::get_buffer(size) == payload);
					CHECK(reader.get_position() == uint64_t(size));
					reader.seek_end(-3);
					CHECK(reader.FileAccess::get_buffer(3) == payload.slice(size - 3));
				}
				fixture.mount();
				Error error = FAILED;
				Ref<FileAccess> reader = FileAccess::open(fixture.logical, FileAccess::READ, &error);
				REQUIRE(reader.is_valid());
				CHECK(error == OK);
				CHECK(reader->get_buffer(size) == payload);
			}
		}
	}
}
TEST_CASE("[F08][PCK] Sparse entries retain plaintext compressed-size metadata") {
	const Vector<uint8_t> payload = bytes(2051);
	for (bool compressed_entry : { false, true }) {
		const Vector<uint8_t> stored = compressed_entry ? compressed(payload) : payload;
		for (bool encrypted_entry : { false, true }) {
			Fixture fixture;
			Ref<FileAccess> output = FileAccess::open(fixture.filename, FileAccess::WRITE);
			REQUIRE(output.is_valid());
			if (encrypted_entry) {
				Vector<uint8_t> key;
				REQUIRE(key.resize(32) == OK);
				memcpy(key.ptrw(), script_encryption_key, 32);
				FileAccessEncrypted writer;
				REQUIRE(writer.open_and_parse(output, key, FileAccessEncrypted::MODE_WRITE_AES256, false) == OK);
				REQUIRE(writer.store_buffer(stored.ptr(), stored.size()));
				writer.close();
			} else {
				REQUIRE(output->store_buffer(stored.ptr(), stored.size()));
			}
			output->close();
			fixture.info.bundle = true;
			fixture.info.size = payload.size();
			fixture.info.stored_size = stored.size();
			fixture.info.lzma2 = compressed_entry;
			fixture.info.encrypted = encrypted_entry;
#ifdef TOOLS_ENABLED
			const String logical = "res://entry.bin";
#else
			const String logical = fixture.filename;
#endif
			for (int repeat = 0; repeat < 2; repeat++) {
				FileAccessPack reader(logical, fixture.info);
				REQUIRE(reader.is_open());
				CHECK(reader.FileAccess::get_buffer(payload.size()) == payload);
			}
			if (encrypted_entry && compressed_entry) {
				fixture.info.stored_size--;
				ErrorsOff quiet;
				FileAccessPack mismatched(logical, fixture.info);
				CHECK_FALSE(mismatched.is_open());
				CHECK(mismatched.get_error() == ERR_FILE_CORRUPT);
			}
		}
	}
}
TEST_CASE("[F08][PCK] Encrypted compressed parsing cannot exceed the declared physical entry") {
	const Vector<uint8_t> payload = bytes(1031);
	Fixture fixture;
	fixture.write(compressed(payload), payload.size(), true, true);
	const uint64_t physical_size = fixture.info.stored_size;
	REQUIRE(physical_size > 40);
	ErrorsOff quiet;
	for (uint64_t extent : { uint64_t(0), uint64_t(39), uint64_t(40), physical_size - 1, physical_size + 1, UINT64_MAX }) {
		fixture.info.stored_size = extent;
		FileAccessPack reader(fixture.logical, fixture.info);
		CHECK_FALSE(reader.is_open());
		CHECK(reader.get_error() == ERR_FILE_CORRUPT);
		uint8_t output = 0xa7;
		CHECK(reader.get_buffer(&output, 1) == 0);
		CHECK(output == 0xa7);
	}
	fixture.info.stored_size = physical_size;
	FileAccessPack reader(fixture.logical, fixture.info);
	REQUIRE(reader.is_open());
	CHECK(reader.FileAccess::get_buffer(payload.size()) == payload);
}

TEST_CASE("[F08][PCK] Existing version 5 directory layout loads encrypted compressed entries with embedded offsets") {
	for (int prefix : { 0, 17 }) {
		Fixture fixture;
		const Vector<uint8_t> payload = bytes(3119);
		fixture.write(compressed(payload), payload.size(), true, true);
		const Vector<uint8_t> encrypted = FileAccess::get_file_as_bytes(fixture.filename).slice(1);
		fixture.logical = "res://f08_legacy_" + uitos(OS::get_singleton()->get_ticks_usec()) + ".bin";
		Ref<FileAccess> pack = FileAccess::open(fixture.filename, FileAccess::WRITE);
		REQUIRE(pack.is_valid());
		for (int i = 0; i < prefix; i++) { REQUIRE(pack->store_8(0xa7)); }
		// Match the pre-remediation V5 exporter layout: physical encrypted
		// stored_size excludes the outer entry padding.
		REQUIRE(pack->store_32(PACK_HEADER_MAGIC));
		REQUIRE(pack->store_32(PACK_FORMAT_VERSION_V5));
		REQUIRE(pack->store_32(4));
		REQUIRE(pack->store_32(0));
		REQUIRE(pack->store_32(0));
		REQUIRE(pack->store_32(PACK_REL_FILEBASE));
		REQUIRE(pack->store_64(104));
		REQUIRE(pack->store_64(104 + encrypted.size() + 5));
		for (int i = 0; i < 16; i++) { REQUIRE(pack->store_32(0)); }
		REQUIRE(pack->get_position() == uint64_t(prefix + 104));
		REQUIRE(pack->store_buffer(encrypted.ptr(), encrypted.size()));
		for (int i = 0; i < 5; i++) { REQUIRE(pack->store_8(0)); }
		REQUIRE(pack->store_32(1));
		const CharString path = fixture.logical.trim_prefix("res://").utf8();
		REQUIRE(pack->store_32(path.length()));
		REQUIRE(pack->store_buffer(reinterpret_cast<const uint8_t *>(path.ptr()), path.length()));
		REQUIRE(pack->store_64(0));
		REQUIRE(pack->store_64(payload.size()));
		REQUIRE(pack->store_64(encrypted.size()));
		uint8_t digest[16];
		REQUIRE(CryptoCore::md5(payload.ptr(), payload.size(), digest) == OK);
		REQUIRE(pack->store_buffer(digest, sizeof(digest)));
		REQUIRE(pack->store_32(PACK_FILE_ENCRYPTED | PACK_FILE_LZMA2));
		pack->close();
		REQUIRE(PackedData::get_singleton()->add_pack(fixture.filename, true, prefix) == OK);
		fixture.registered = true;
		for (int repeat = 0; repeat < 2; repeat++) {
			Error error = FAILED;
			Ref<FileAccess> reader = FileAccess::open(fixture.logical, FileAccess::READ, &error);
			REQUIRE(error == OK);
			REQUIRE(reader.is_valid());
			CHECK(reader->get_length() == uint64_t(payload.size()));
			CHECK(reader->get_buffer(payload.size()) == payload);
		}
	}
}
TEST_CASE("[PCKEncoderDiagnostic] Current SDK encoder round trips an ordinary payload") {
	const Vector<uint8_t> payload = bytes(1031);
	PCKLzmaOptions options;
	options.dictionary_size_mb = 1;
	Vector<uint8_t> encoded;
	REQUIRE(compress_lzma2(payload, encoded, options) == OK);
	Vector<uint8_t> decoded;
	REQUIRE(decompress_lzma2(encoded.ptr(), encoded.size(), payload.size(), decoded) == OK);
	CHECK(decoded == payload);
}

TEST_CASE("[B01Allocation] Decoder sizing and invalid input return bounded errors") {
	const Vector<uint8_t> payload = bytes(1031);
	const Vector<uint8_t> encoded = compressed(payload);
	Vector<uint8_t> output;
	REQUIRE(decompress_lzma2(encoded.ptr(), encoded.size(), payload.size(), output) == OK);
	CHECK(output == payload);
	ErrorsOff quiet;
	CHECK(decompress_lzma2(encoded.ptr(), encoded.size(), uint64_t(INT32_MAX) + 1, output) == ERR_INVALID_PARAMETER);
	CHECK(output.is_empty());
	CHECK(decompress_lzma2(nullptr, 0, 1, output) == ERR_INVALID_PARAMETER);
	CHECK(decompress_lzma2(encoded.ptr(), 0, 1, output) == ERR_FILE_CORRUPT);
#if SIZE_MAX < UINT64_MAX
	CHECK(decompress_lzma2(encoded.ptr(), uint64_t(SIZE_MAX) + 1, 1, output) == ERR_INVALID_PARAMETER);
#endif
	CHECK(decompress_lzma2(encoded.ptr(), encoded.size() - 1, payload.size(), output) != OK);
	CHECK(output.is_empty());
}

TEST_CASE("[B01Init] Failed compressed initialization never looks open") {
	Fixture fixture;
	const Vector<uint8_t> payload = bytes(4099);
	Vector<uint8_t> encoded = compressed(payload);
	encoded.resize(encoded.size() - 1);
	fixture.write(encoded, payload.size(), true);
	ErrorsOff quiet;
	FileAccessPack pack(fixture.logical, fixture.info);
	CHECK_FALSE(pack.is_open());
	CHECK(pack.get_error() != OK);
	CHECK(pack.get_position() == 0);
	uint8_t output = 0xa7;
	CHECK(pack.get_buffer(&output, 1) == 0);
	CHECK(output == 0xa7);
	pack.seek(0);
	CHECK_FALSE(pack.is_open());
	pack.close();
	pack.close();
	CHECK_FALSE(pack.is_open());
}

TEST_CASE("[B01Init] Failed bound entries do not fall through to a loose file") {
	Fixture fixture;
	fixture.write(bytes(1), 1000, true);
	Ref<FileAccess> loose = FileAccess::open(fixture.logical, FileAccess::WRITE);
	REQUIRE(loose.is_valid());
	REQUIRE(loose->store_buffer(bytes(4).ptr(), 4));
	loose.unref();
	fixture.mount();
	ErrorsOff quiet;
	Error error = OK;
	CHECK(FileAccess::open(fixture.logical, FileAccess::READ, &error).is_null());
	CHECK(error != OK);
	CHECK(PackedData::get_singleton()->try_open_path(fixture.logical).is_null());
	PackedData::get_singleton()->remove_path(fixture.logical);
	fixture.registered = false;
	CHECK(FileAccess::open(fixture.logical, FileAccess::READ, &error).is_valid());
	CHECK(error == OK);
}

TEST_CASE("[B01Init] Missing backing and oversized staging fail before reads") {
	Fixture missing;
	missing.info.size = 10;
	missing.info.stored_size = 10;
	missing.info.lzma2 = true;
	ErrorsOff quiet;
	FileAccessPack absent(missing.logical, missing.info);
	CHECK_FALSE(absent.is_open());
	CHECK(absent.get_error() != OK);
	Fixture huge;
	huge.write(bytes(1), 10, true);
	huge.info.stored_size = uint64_t(INT32_MAX) + 1;
	FileAccessPack invalid(huge.logical, huge.info);
	CHECK_FALSE(invalid.is_open());
	CHECK(invalid.get_error() != OK);
}

TEST_CASE("[B01Compatibility] Successful plain compressed and encrypted entries reopen") {
	const Vector<uint8_t> payload = bytes(2051);
	for (bool use_compression : { false, true }) {
		Fixture fixture;
		fixture.write(use_compression ? compressed(payload) : payload, payload.size(), use_compression);
		fixture.mount();
		for (int repeat = 0; repeat < 3; repeat++) {
			Error error = FAILED;
			Ref<FileAccess> pack = FileAccess::open(fixture.logical, FileAccess::READ, &error);
			REQUIRE(pack.is_valid());
			CHECK(error == OK);
			CHECK(pack->get_buffer(pack->get_length()) == payload);
			pack->seek(0);
			CHECK(pack->get_buffer(17) == payload.slice(0, 17));
		}
	}
	Fixture encrypted;
	encrypted.write(payload, payload.size(), false, true);
	FileAccessPack pack(encrypted.logical, encrypted.info);
	REQUIRE(pack.is_open());
	CHECK(pack.FileAccess::get_buffer(payload.size()) == payload);
}

TEST_CASE("[B01Compatibility] Empty raw entries have normal EOF behaviour") {
	Fixture fixture;
	fixture.write(Vector<uint8_t>(), 0);
	FileAccessPack pack(fixture.logical, fixture.info);
	REQUIRE(pack.is_open());
	CHECK(pack.get_error() == OK);
	uint8_t value = 0x41;
	CHECK(pack.get_buffer(&value, 0) == 0);
	CHECK(pack.get_buffer(&value, 1) == 0);
	CHECK(pack.eof_reached());
	pack.seek(0);
	CHECK_FALSE(pack.eof_reached());
	CHECK(pack.get_error() == OK);
}

TEST_CASE("[B01Read] Short backing data reports actual bytes and position") {
	Fixture fixture;
	fixture.write(bytes(3), 12);
	FileAccessPack pack(fixture.logical, fixture.info);
	uint8_t output[12];
	memset(output, 0xa7, sizeof(output));
	CHECK(pack.get_buffer(output, sizeof(output)) == 3);
	CHECK(pack.get_position() == 3);
	CHECK(pack.eof_reached());
	CHECK(pack.get_error() != OK);
	for (int i = 3; i < 12; i++) { CHECK(output[i] == 0xa7); }
	pack.seek(0);
	CHECK(pack.get_position() == 0);
	CHECK_FALSE(pack.eof_reached());
	CHECK(pack.FileAccess::get_buffer(12).size() == 3);
}

class PartialReader : public FileAccessMemory {
public:
	mutable int calls = 0;
	uint64_t limit = 2;
	bool fail = false;
	bool stalled = false;
	bool closed = false;
	bool is_open() const override { return !closed && FileAccessMemory::is_open(); }
	uint64_t get_buffer(uint8_t *p_dst, uint64_t p_length) const override {
		calls++;
		if (fail) { return uint64_t(-1); }
		if (stalled) { return 0; }
		return FileAccessMemory::get_buffer(p_dst, MIN(limit, p_length));
	}
	Error get_error() const override { return fail ? ERR_FILE_CANT_READ : OK; }
};

TEST_CASE("[B01Read] Partial progress is retryable and I/O errors are preserved") {
	Fixture fixture;
	const Vector<uint8_t> payload = bytes(7);
	fixture.write(payload, payload.size());
	FileAccessPack pack(fixture.logical, fixture.info);
	Ref<PartialReader> reader;
	reader.instantiate();
	REQUIRE(reader->open_custom(payload.ptr(), payload.size()) == OK);
	Access::backing(pack, reader);
	pack.seek(0);
	uint8_t output[7] = {};
	CHECK(pack.get_buffer(output, 7) == 2);
	CHECK(pack.get_position() == 2);
	CHECK_FALSE(pack.eof_reached());
	CHECK(pack.get_error() == OK);
	reader->fail = true;
	CHECK(pack.get_buffer(output + 2, 5) == 0);
	CHECK(pack.get_position() == 2);
	CHECK(pack.get_error() == ERR_FILE_CANT_READ);
	reader->fail = false;
	pack.seek(0);
	CHECK(pack.get_error() == OK);
	CHECK(pack.get_buffer(output, 7) == 2);
	reader->stalled = true;
	CHECK(pack.get_buffer(output, 1) == 0);
	CHECK(pack.get_error() == ERR_FILE_CANT_READ);
}

TEST_CASE("[B01Read] Logical boundaries and extreme requests never wrap") {
	Fixture fixture;
	const Vector<uint8_t> payload = bytes(7);
	fixture.write(payload, payload.size());
	FileAccessPack pack(fixture.logical, fixture.info);
	uint8_t output[7] = {};
	CHECK(pack.get_buffer(output, UINT64_MAX) == 7);
	CHECK(pack.get_position() == 7);
	CHECK(pack.eof_reached());
	pack.seek(0);
	CHECK(pack.get_error() == OK);
	CHECK(pack.get_buffer(output, 7) == 7);
	CHECK_FALSE(pack.eof_reached());
	CHECK(pack.get_buffer(output, 1) == 0);
	CHECK(pack.eof_reached());
	pack.seek_end(-2);
	CHECK(pack.get_position() == 5);
	CHECK(pack.get_buffer(output, 2) == 2);
	ErrorsOff quiet;
	const uint64_t previous = pack.get_position();
	pack.seek_end(INT64_MIN);
	CHECK(pack.get_position() == previous);
	CHECK(pack.get_error() != OK);
	pack.seek(0);
	CHECK(pack.get_error() == OK);
}

TEST_CASE("[B01Read] Offset overflow and closed backing never report progress") {
	Fixture fixture;
	const Vector<uint8_t> payload = bytes(7);
	fixture.write(payload, UINT64_MAX);
	FileAccessPack pack(fixture.logical, fixture.info);
	ErrorsOff quiet;
	pack.seek(UINT64_MAX);
	CHECK(pack.get_position() == 0);
	CHECK(pack.get_error() == ERR_INVALID_PARAMETER);
	pack.seek(0);
	CHECK(pack.get_error() == OK);
	Ref<PartialReader> reader;
	reader.instantiate();
	REQUIRE(reader->open_custom(payload.ptr(), payload.size()) == OK);
	Access::backing(pack, reader);
	reader->closed = true;
	uint8_t output = 0xa7;
	CHECK(pack.get_buffer(&output, 1) == 0);
	CHECK(pack.get_position() == 0);
	CHECK(output == 0xa7);
	CHECK(pack.get_error() == ERR_FILE_CANT_READ);
}

TEST_CASE("[B01Init] Sparse backing and truncated encrypted data fail closed") {
	Fixture sparse;
	sparse.info.bundle = true;
	sparse.info.size = 17;
	ErrorsOff quiet;
	FileAccessPack absent("res://b01_missing_sparse.bin", sparse.info);
	CHECK_FALSE(absent.is_open());
	CHECK(absent.get_error() != OK);
	Fixture encrypted;
	encrypted.write(bytes(64), 64, false, true);
	Ref<FileAccess> file = FileAccess::open(encrypted.filename, FileAccess::READ_WRITE);
	REQUIRE(file.is_valid());
	REQUIRE(file->resize(file->get_length() - 1) == OK);
	file.unref();
	encrypted.mount();
	Error error = OK;
	CHECK(FileAccess::open(encrypted.logical, FileAccess::READ, &error).is_null());
	CHECK(error != OK);
}

TEST_CASE("[B01Compatibility] Cold and warm cached reads keep exact logical boundaries") {
	Fixture fixture;
	const Vector<uint8_t> payload = bytes(3119);
	fixture.write(compressed(payload), payload.size(), true);
	fixture.mount();
	for (int repeat = 0; repeat < 2; repeat++) {
		Ref<FileAccess> pack = FileAccess::open(fixture.logical, FileAccess::READ);
		REQUIRE(pack.is_valid());
		Vector<uint8_t> output;
		output.resize(payload.size());
		CHECK(pack->get_buffer(output.ptrw(), UINT64_MAX) == uint64_t(payload.size()));
		CHECK(output == payload);
		CHECK(pack->eof_reached());
		pack->seek_end(-7);
		CHECK(pack->get_error() == OK);
		CHECK(pack->get_buffer(7) == payload.slice(payload.size() - 7));
	}
}

TEST_CASE("[B01Init] A rejected initial backing seek cannot open an entry") {
	Fixture fixture;
	fixture.write(bytes(7), 7);
	fixture.info.offset = uint64_t(1) << 63;
	ErrorsOff quiet;
	for (int repeat = 0; repeat < 2; repeat++) {
		FileAccessPack pack(fixture.logical, fixture.info);
		CHECK_FALSE(pack.is_open());
		CHECK(pack.get_error() != OK);
		CHECK(pack.get_position() == 0);
		uint8_t output = 0xa7;
		CHECK(pack.get_buffer(&output, 1) == 0);
		CHECK(output == 0xa7);
	}
}

TEST_CASE("[B01Read] A rejected backing seek preserves the logical position") {
	Fixture fixture;
	const Vector<uint8_t> payload = bytes(7);
	fixture.write(payload, UINT64_MAX);
	FileAccessPack pack(fixture.logical, fixture.info);
	REQUIRE(pack.is_open());
	ErrorsOff quiet;
	pack.seek(uint64_t(1) << 63);
	CHECK(pack.get_position() == 0);
	CHECK(pack.get_error() != OK);
	pack.seek(0);
	CHECK(pack.get_error() == OK);
	CHECK(pack.FileAccess::get_buffer(payload.size()) == payload);
}

#ifdef B01_ALLOCATION_TESTS
static thread_local bool allocation_armed = false;
static thread_local size_t allocation_minimum = 0;
static thread_local size_t allocation_maximum = SIZE_MAX;
static thread_local int allocation_failures = 0;
static thread_local bool allocation_recording = false;
static thread_local size_t first_allocation_size = 0;
static bool fail_allocation(size_t p_size) {
	if (allocation_recording && first_allocation_size == 0) {
		first_allocation_size = p_size;
	}
	if (allocation_armed && p_size >= allocation_minimum && p_size <= allocation_maximum) {
		allocation_armed = false;
		allocation_failures++;
		return true;
	}
	return false;
}
struct AllocationFailure {
	AllocationFailure(size_t p_min = 0, size_t p_max = SIZE_MAX) {
		allocation_failures = 0;
		allocation_minimum = p_min;
		allocation_maximum = p_max;
		allocation_armed = true;
	}
	~AllocationFailure() { allocation_armed = false; }
};

static size_t vector_allocation_size(int64_t p_size) {
	Vector<uint8_t> probe;
	first_allocation_size = 0;
	allocation_recording = true;
	const Error error = probe.resize(p_size);
	allocation_recording = false;
	REQUIRE(error == OK);
	REQUIRE(first_allocation_size >= size_t(p_size));
	return first_allocation_size;
}


TEST_CASE("[F14][PCKCacheAllocation] A warm accepted entry reuses decoded storage") {
	ScopedPackedData isolated;
	Fixture fixture;
	const Vector<uint8_t> payload = bytes(100003);
	fixture.write(compressed(payload), payload.size(), true);
	fixture.mount();
	Ref<FileAccess> cold = FileAccess::open(fixture.logical, FileAccess::READ);
	REQUIRE(cold.is_valid());
	CHECK(cold->get_buffer(payload.size()) == payload);
	const size_t exact = vector_allocation_size(payload.size());
	Ref<FileAccess> warm;
	Error error = FAILED;
	{
		AllocationFailure fault(exact, exact);
		warm = FileAccess::open(fixture.logical, FileAccess::READ, &error);
	}
	CHECK(allocation_failures == 0);
	REQUIRE(error == OK);
	REQUIRE(warm.is_valid());
	CHECK(warm->get_buffer(payload.size()) == payload);
}

TEST_CASE("[N002][PCKDeltaAllocation] Whole-base staging allocation failure stays explicit and retry succeeds") {
	ScopedPackedData isolated;
	Fixture base;
	Fixture patch;
	MountedPath mounted(base.logical);
	const Vector<uint8_t> original = bytes(100003);
	Vector<uint8_t> expected = original;
	expected.write[17] ^= 0xa7;
	base.write(original, original.size());
	base.mount();
	const Vector<uint8_t> delta = encoded_delta(original, expected);
	patch.write(delta, delta.size());
	patch.info.delta = true;
	map_entry(base.logical, patch.info);
	Ref<FileAccess> reader = FileAccess::open(base.logical, FileAccess::READ);
	REQUIRE(reader.is_valid());
	const size_t exact = vector_allocation_size(original.size());
	uint64_t length = UINT64_MAX;
	ErrorsOff quiet;
	{
		AllocationFailure fault(exact, exact);
		length = reader->get_length();
	}
	CHECK(allocation_failures == 1);
	CHECK(length == 0);
	CHECK(reader->get_error() == ERR_OUT_OF_MEMORY);
	uint8_t output = 0xa7;
	CHECK(reader->get_buffer(&output, 1) == 0);
	CHECK(output == 0xa7);
	CHECK(reader->get_error() == ERR_OUT_OF_MEMORY);
	Ref<FileAccess> retry = FileAccess::open(base.logical, FileAccess::READ);
	REQUIRE(retry.is_valid());
	CHECK(retry->get_length() == uint64_t(expected.size()));
	CHECK(retry->get_buffer(expected.size()) == expected);
}

TEST_CASE("[N009][PCKKeyAllocation] Failed default and compiled fallback allocations never publish a key") {
	ErrorsOff quiet;
	uint8_t compiled[32];
	constexpr uint8_t empty[32] = {};
	for (int i = 0; i < 32; i++) { compiled[i] = i; }
	const Vector<uint8_t> provided = bytes(32);
	const size_t exact = vector_allocation_size(32);
	for (int mode = 0; mode < 2; mode++) {
		Vector<uint8_t> selected = provided;
		const Vector<uint8_t> override_key = mode ? provided : Vector<uint8_t>();
		Error error;
		{
			AllocationFailure fault(exact, exact);
			error = mode ? PCKEncryption::select_key(override_key, compiled, true, selected) :
					PCKEncryption::select_key(override_key, empty, true, selected);
		}
		CHECK(allocation_failures == 1);
		CHECK(error == ERR_OUT_OF_MEMORY);
		CHECK(selected.is_empty());
		CHECK(PCKEncryption::select_key(override_key, compiled, true, selected) == OK);
		REQUIRE(selected.size() == 32);
		CHECK(memcmp(selected.ptr(), compiled, 32) == 0);
	}
}
TEST_CASE("[B01AllocationFault] Real allocator destination failure returns an empty result") {
	const Vector<uint8_t> payload = bytes(5003);
	const Vector<uint8_t> encoded = compressed(payload);
	Vector<uint8_t> output;
	ErrorsOff quiet;
	Error result;
	{
		AllocationFailure fault;
		result = decompress_lzma2(encoded.ptr(), encoded.size(), payload.size(), output);
	}
	CHECK(allocation_failures == 1);
	CHECK(result == ERR_OUT_OF_MEMORY);
	CHECK(output.is_empty());
}

TEST_CASE("[B01AllocationFault] Real allocator staging failure occurs before a backing read") {
	Fixture fixture;
	const Vector<uint8_t> payload = bytes(200003, true);
	fixture.write(compressed(payload), payload.size(), true);
	ErrorsOff quiet;
	{
		const size_t allocation_size = vector_allocation_size(fixture.info.stored_size);
		AllocationFailure fault(allocation_size, allocation_size);
		FileAccessPack pack(fixture.logical, fixture.info);
		CHECK(allocation_failures == 1);
		CHECK_FALSE(pack.is_open());
		CHECK(pack.get_error() == ERR_OUT_OF_MEMORY);
		CHECK((Access::backing_position(pack) == 0 || Access::backing_position(pack) == fixture.info.offset));
	}
}

TEST_CASE("[B01CombinedFault] Allocation failure propagates through packed public open") {
	Fixture fixture;
	const Vector<uint8_t> payload = bytes(200003, true);
	fixture.write(compressed(payload), payload.size(), true);
	fixture.mount();
	ErrorsOff quiet;
	Error error = OK;
	Ref<FileAccess> result;
	{
		const size_t allocation_size = vector_allocation_size(fixture.info.stored_size);
		AllocationFailure fault(allocation_size, allocation_size);
		result = FileAccess::open(fixture.logical, FileAccess::READ, &error);
	}
	CHECK(allocation_failures == 1);
	CHECK(result.is_null());
	CHECK(error == ERR_OUT_OF_MEMORY);
	result = FileAccess::open(fixture.logical, FileAccess::READ, &error);
	REQUIRE(result.is_valid());
	CHECK(error == OK);
	CHECK(result->get_buffer(payload.size()) == payload);
}
TEST_CASE("[B01CombinedFault] Output allocation failure propagates through packed public open") {
	Fixture fixture;
	const Vector<uint8_t> payload = bytes(100003);
	fixture.write(compressed(payload), payload.size(), true);
	fixture.mount();
	ErrorsOff quiet;
	Error error = OK;
	Ref<FileAccess> result;
	{
		const size_t allocation_size = vector_allocation_size(payload.size());
		AllocationFailure fault(allocation_size, allocation_size);
		result = FileAccess::open(fixture.logical, FileAccess::READ, &error);
	}
	CHECK(allocation_failures == 1);
	CHECK(result.is_null());
	CHECK(error == ERR_OUT_OF_MEMORY);
	result = FileAccess::open(fixture.logical, FileAccess::READ, &error);
	REQUIRE(result.is_valid());
	CHECK(error == OK);
	CHECK(result->get_buffer(payload.size()) == payload);
}

#ifdef MODULE_JPG_ENABLED
TEST_CASE("[F02AllocationFault][JPEG] Output allocation failure is propagated and retry succeeds") {
	Ref<Image> source = memnew(Image(257, 113, false, Image::FORMAT_RGB8));
	source->fill(Color(0.25, 0.5, 0.75));
	const Vector<uint8_t> encoded = source->save_jpg_to_buffer(0.9);
	REQUIRE_FALSE(encoded.is_empty());
	Ref<Image> destination = memnew(Image(2, 3, false, Image::FORMAT_L8));
	const Vector<uint8_t> original = destination->get_data();
	const size_t allocation_size = vector_allocation_size(257 * 113 * 3);
	Error result;
	ErrorsOff quiet;
	{
		AllocationFailure fault(allocation_size, allocation_size);
		result = jpeg_turbo_load_image_from_buffer(destination.ptr(), encoded.ptr(), encoded.size());
	}
	CHECK(allocation_failures == 1);
	CHECK(result == ERR_OUT_OF_MEMORY);
	CHECK(destination->get_width() == 2);
	CHECK(destination->get_height() == 3);
	CHECK(destination->get_data() == original);
	REQUIRE(jpeg_turbo_load_image_from_buffer(destination.ptr(), encoded.ptr(), encoded.size()) == OK);
	CHECK(destination->get_data().size() == 257 * 113 * 3);
}

TEST_CASE("[F02AllocationFault][JPEG] Pixel-limit boundary reaches checked allocation without allocating it") {
	const Vector<uint8_t> encoded = FileAccess::get_file_as_bytes(TestUtils::get_data_path("images/icon.jpg"));
	REQUIRE_FALSE(encoded.is_empty());
	const int dimensions[][2] = { { 16384, 16384 }, { 16384, 16383 } };
	for (const auto &size : dimensions) {
		Vector<uint8_t> boundary = encoded;
		REQUIRE(TestJPEG::dimensions(boundary, size[0], size[1]));
		Ref<Image> destination = memnew(Image(2, 3, false, Image::FORMAT_L8));
		const Vector<uint8_t> original = destination->get_data();
		Error result;
		ErrorsOff quiet;
		{
			AllocationFailure fault(size_t(size[0]) * size[1] * 3, SIZE_MAX);
			result = jpeg_turbo_load_image_from_buffer(destination.ptr(), boundary.ptr(), boundary.size());
		}
		CHECK(allocation_failures == 1);
		CHECK(result == ERR_OUT_OF_MEMORY);
		CHECK(destination->get_width() == 2);
		CHECK(destination->get_height() == 3);
		CHECK(destination->get_data() == original);
	}
}
#endif // MODULE_JPG_ENABLED

TEST_CASE("[N008][EncryptedAllocation] IV and payload allocation failures clear state and permit retry") {
	ErrorsOff quiet;
	const Vector<uint8_t> data = TestEncryptedSafety::stream(1031);
	const Vector<uint8_t> key = TestEncryptedSafety::key();
	for (int size : { 16, 1040 }) {
		Ref<FileAccessMemory> source;
		source.instantiate();
		REQUIRE(source->open_custom(data.ptr(), data.size()) == OK);
		FileAccessEncrypted reader;
		const size_t allocation_size = vector_allocation_size(size);
		Error error;
		{
			AllocationFailure fault(allocation_size, allocation_size);
			error = reader.open_and_parse(source, key, FileAccessEncrypted::MODE_READ);
		}
		CHECK(allocation_failures == 1);
		CHECK(error == ERR_OUT_OF_MEMORY);
		CHECK_FALSE(reader.is_open());
		CHECK(reader.get_length() == 0);
		CHECK(reader.get_iv().is_empty());
		CHECK(source->get_position() <= 44);
		source->seek(0);
		REQUIRE(reader.open_and_parse(source, key, FileAccessEncrypted::MODE_READ) == OK);
		CHECK(reader.get_length() == 1031);
	}
}
TEST_CASE("[N008][EncryptedAllocation] Password key allocation fails before parser input is consumed") {
	ErrorsOff quiet;
	const Vector<uint8_t> data = TestEncryptedSafety::stream(17);
	Ref<FileAccessMemory> source;
	source.instantiate();
	REQUIRE(source->open_custom(data.ptr(), data.size()) == OK);
	FileAccessEncrypted reader;
	REQUIRE(reader.open_and_parse(source, TestEncryptedSafety::key(), FileAccessEncrypted::MODE_READ) == OK);
	reader.close();
	source->seek(0);
	const size_t allocation_size = vector_allocation_size(32);
	Error error;
	{
		AllocationFailure fault(allocation_size, allocation_size);
		error = reader.open_and_parse_password(source, "ordinary-test-password", FileAccessEncrypted::MODE_READ);
	}
	CHECK(allocation_failures == 1);
	CHECK(error == ERR_OUT_OF_MEMORY);
	CHECK_FALSE(reader.is_open());
	CHECK(reader.get_length() == 0);
	CHECK(reader.get_iv().is_empty());
	CHECK(source->get_position() == 0);
}
#endif


#if defined(B01_ALLOCATION_TESTS) && defined(MODULE_WEBP_ENABLED)
TEST_CASE("[F07][WebPAllocation] Static destination failures preserve the prior image") {
	const Vector<uint8_t> encoded = TestWebPSafety::make_static(false);
	Ref<Image> image = memnew(Image(2, 3, false, Image::FORMAT_L8));
	const Vector<uint8_t> previous = image->get_data();
	const size_t exact = vector_allocation_size(17 * 19 * 3);
	Error error = OK;
	ErrorsOff quiet;
	{
		AllocationFailure fault(exact, exact);
		error = WebPCommon::webp_load_image_from_buffer(image.ptr(), encoded.ptr(), encoded.size());
	}
	CHECK(allocation_failures == 1);
	CHECK(error == ERR_OUT_OF_MEMORY);
	CHECK(image->get_data() == previous);
	CHECK(image->get_width() == 2);
	CHECK(image->get_height() == 3);
	CHECK(WebPCommon::webp_load_image_from_buffer(image.ptr(), encoded.ptr(), encoded.size()) == OK);
	Ref<Image> unpacked;
	{
		AllocationFailure fault(exact, exact);
		unpacked = WebPCommon::_webp_unpack(encoded);
	}
	CHECK(allocation_failures == 1);
	CHECK(unpacked.is_null());
	CHECK(WebPCommon::_webp_unpack(encoded).is_valid());
}

TEST_CASE("[SceneTree][F07][WebPAllocation] Metadata and resource input failures remain explicit") {
	const Vector<uint8_t> encoded = TestWebPSafety::make_animation(256);
	TestWebPSafety::Fixture fixture(encoded);
	uint32_t count = 0;
	REQUIRE(ResourceFormatWebP::get_webp_frame_count(fixture.path, count) == OK);
	const size_t exact = vector_allocation_size(encoded.size());
	Error error = OK;
	ErrorsOff quiet;
	{
		AllocationFailure fault(exact, exact);
		error = ResourceFormatWebP::get_webp_frame_count(fixture.path, count);
	}
	CHECK(allocation_failures == 1);
	CHECK(error == ERR_OUT_OF_MEMORY);
	CHECK(count == 0);
	Ref<Resource> result;
	{
		AllocationFailure fault(exact, exact);
		result = ResourceFormatWebP::load_texture(fixture.path, &error);
	}
	CHECK(allocation_failures == 1);
	CHECK(error == ERR_OUT_OF_MEMORY);
	CHECK(result.is_null());
	CHECK(ResourceFormatWebP::load_texture(fixture.path, &error).is_valid());
	CHECK(error == OK);
}

TEST_CASE("[SceneTree][F07][WebPAllocation] Animated canvas copy failure cleans up and permits retry") {
	const Vector<uint8_t> encoded = TestWebPSafety::make_animation(2);
	TestWebPSafety::Fixture fixture(encoded);
	Error error = FAILED;
	Ref<Resource> control = ResourceFormatWebP::load_texture(fixture.path, &error);
	REQUIRE(control.is_valid());
	REQUIRE(error == OK);
	const size_t exact = vector_allocation_size(17 * 19 * 4);
	REQUIRE(exact != vector_allocation_size(encoded.size()));
	Ref<Resource> result;
	ErrorsOff quiet;
	{
		AllocationFailure fault(exact, exact);
		result = ResourceFormatWebP::load_texture(fixture.path, &error);
	}
	CHECK(allocation_failures == 1);
	CHECK(error == ERR_OUT_OF_MEMORY);
	CHECK(result.is_null());
	CHECK(ResourceFormatWebP::load_texture(fixture.path, &error).is_valid());
	CHECK(error == OK);
}
#endif

} // namespace TestPCKSafety

#ifdef B01_ALLOCATION_TESTS
extern "C" {
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__wrap_malloc(size_t p_size) {
	return TestPCKSafety::fail_allocation(p_size) ? nullptr : __real_malloc(p_size);
}
void *__wrap_calloc(size_t p_count, size_t p_size) {
	return p_count && p_size > SIZE_MAX / p_count ? nullptr : (TestPCKSafety::fail_allocation(p_count * p_size) ? nullptr : __real_calloc(p_count, p_size));
}
void *__wrap_realloc(void *p_pointer, size_t p_size) {
	return TestPCKSafety::fail_allocation(p_size) ? nullptr : __real_realloc(p_pointer, p_size);
}
}
#endif
