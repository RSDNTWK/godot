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
#include "core/io/file_access_encrypted.h"
#include "core/io/file_access_memory.h"
#include "core/io/file_access_pack.h"
#include "core/io/pck_lzma.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "tests/test_utils.h"

#include <cstring>
#include <limits>

namespace TestPCKSafety {

struct Access {
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
			REQUIRE(encrypted->open_and_parse(file, key, FileAccessEncrypted::MODE_WRITE, false) == OK);
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
	PCKLzmaOptions options;
	options.dictionary_size_mb = 1;
	Vector<uint8_t> output;
	REQUIRE(compress_lzma2(p_input, output, options) == OK);
	return output;
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

#ifdef B01_ALLOCATION_TESTS
static thread_local bool allocation_armed = false;
static thread_local size_t allocation_minimum = 0;
static thread_local size_t allocation_maximum = SIZE_MAX;
static thread_local int allocation_failures = 0;
static bool fail_allocation(size_t p_size) {
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
		AllocationFailure fault(250000, 300000);
		FileAccessPack pack(fixture.logical, fixture.info);
		CHECK(allocation_failures == 1);
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
		AllocationFailure fault(250000, 300000);
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
