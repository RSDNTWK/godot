/**************************************************************************/
/*  test_compressed_writer_safety.cpp                                     */
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
TEST_FORCE_LINK(test_compressed_writer_safety)
#include "core/io/dir_access.h"
#include "core/io/file_access_compressed.h"
#include "core/os/os.h"
#include "tests/core/io/allocation_test_helpers.h"
#include "tests/test_utils.h"

#include <cstring>

namespace TestCompressedWriterSafety {
struct Fixture {
	static inline uint64_t sequence = 0;
	String path = TestUtils::get_temp_path("n037_" + uitos(OS::get_singleton()->get_ticks_usec()) + "_" + uitos(sequence++) + ".gcmp");
	~Fixture() {
		if (FileAccess::exists(path)) {
			DirAccess::remove_absolute(path);
		}
	}
};
static Vector<uint8_t> payload(int p_size) {
	Vector<uint8_t> result;
	REQUIRE(result.resize(p_size) == OK);
	for (int i = 0; i < p_size; i++) {
		result.write[i] = i % 251;
	}
	return result;
}
TEST_CASE("[N037][CompressedWriter] Empty tiny complete and final blocks retain the existing format") {
	for (Compression::Mode mode : { Compression::MODE_FASTLZ, Compression::MODE_GZIP, Compression::MODE_LZMA2 }) {
		for (int length : { 0, 1, 32, 33, 513 }) {
			Fixture fixture;
			const Vector<uint8_t> plain = payload(length);
			FileAccessCompressed writer;
			writer.configure("GCMP", mode, 32);
			REQUIRE(writer.open_internal(fixture.path, FileAccess::WRITE) == OK);
			CHECK(writer.store_buffer(plain.ptr(), plain.size()));
			CHECK(writer.get_length() == uint64_t(length));
			writer.close();
			FileAccessCompressed reader;
			REQUIRE(reader.open_internal(fixture.path, FileAccess::READ) == OK);
			CHECK(reader.get_length() == uint64_t(length));
			Vector<uint8_t> decoded;
			REQUIRE(decoded.resize(MAX(length, 1)) == OK);
			CHECK(reader.get_buffer(decoded.ptrw(), length) == uint64_t(length));
			if (length > 0) {
				CHECK(memcmp(decoded.ptr(), plain.ptr(), length) == 0);
			}
			reader.close();
		}
	}
}
TEST_CASE("[N037][CompressedWriter] Unrepresentable write extents reject before changing position or length") {
	Fixture fixture;
	FileAccessCompressed writer;
	writer.configure("GCMP", Compression::MODE_GZIP, 32);
	REQUIRE(writer.open_internal(fixture.path, FileAccess::WRITE) == OK);
	const uint8_t byte = 7;
	CHECK_FALSE(writer.store_buffer(&byte, UINT64_MAX));
	CHECK(writer.get_error() == ERR_INVALID_PARAMETER);
	CHECK(writer.get_position() == 0);
	CHECK(writer.get_length() == 0);
	writer.close();
	CHECK(writer.get_error() == ERR_INVALID_PARAMETER);
}
#ifdef B01_ALLOCATION_TESTS
struct QuietErrors {
	bool previous = CoreGlobals::print_error_enabled;
	QuietErrors() { CoreGlobals::print_error_enabled = false; }
	~QuietErrors() { CoreGlobals::print_error_enabled = previous; }
};
TEST_CASE("[N037][CompressedWriterAllocation] Failed initial allocation does not open or truncate the backing file") {
	Fixture fixture;
	FileAccessCompressed writer;
	writer.configure("GCMP", Compression::MODE_GZIP, 32);
	const size_t allocation = TestAllocation::allocation_size([]() {
		Vector<uint8_t> initial;
		REQUIRE(initial.resize(256) == OK);
	});
	QuietErrors quiet;
	{
		TestAllocation::Failure failure(allocation);
		CHECK(writer.open_internal(fixture.path, FileAccess::WRITE) == ERR_OUT_OF_MEMORY);
		CHECK(TestAllocation::failures() == 1);
	}
	CHECK_FALSE(writer.is_open());
	CHECK_FALSE(FileAccess::exists(fixture.path));
	writer.close();
	CHECK(writer.get_error() == ERR_OUT_OF_MEMORY);
}
TEST_CASE("[N037][CompressedWriterAllocation] Growth failure leaves successful extents unchanged and survives close or destruction") {
	for (bool explicit_close : { false, true }) {
		Fixture fixture;
		const Vector<uint8_t> plain = payload(513);
		Vector<uint8_t> probe;
		REQUIRE(probe.resize(256) == OK);
		const size_t allocation = TestAllocation::allocation_size([&]() {
			REQUIRE(probe.resize(1024) == OK);
		});
		{
			FileAccessCompressed writer;
			writer.configure("GCMP", Compression::MODE_GZIP, 32);
			REQUIRE(writer.open_internal(fixture.path, FileAccess::WRITE) == OK);
			REQUIRE(writer.store_buffer(plain.ptr(), 32));
			QuietErrors quiet;
			{
				TestAllocation::Failure failure(allocation);
				CHECK_FALSE(writer.store_buffer(plain.ptr(), plain.size()));
				CHECK(TestAllocation::failures() == 1);
			}
			CHECK(writer.get_position() == 32);
			CHECK(writer.get_length() == 32);
			CHECK(writer.get_error() == ERR_OUT_OF_MEMORY);
			CHECK_FALSE(writer.store_buffer(plain.ptr(), plain.size()));
			if (explicit_close) {
				writer.close();
				CHECK(writer.get_error() == ERR_OUT_OF_MEMORY);
				writer.close();
				CHECK(writer.get_error() == ERR_OUT_OF_MEMORY);
				REQUIRE(writer.open_internal(fixture.path, FileAccess::WRITE) == OK);
				CHECK(writer.get_error() == OK);
				REQUIRE(writer.store_buffer(plain.ptr(), plain.size()));
			}
		}
		Ref<FileAccess> backing = FileAccess::open(fixture.path, FileAccess::READ);
		REQUIRE(backing.is_valid());
		CHECK((backing->get_length() > 0) == explicit_close);
	}
}
TEST_CASE("[N037][CompressedWriterAllocation] Close allocation failures report errors and always release the writer") {
	for (bool table : { false, true }) {
		Fixture fixture;
		const Vector<uint8_t> plain = payload(513);
		FileAccessCompressed writer;
		writer.configure("GCMP", Compression::MODE_GZIP, 32);
		REQUIRE(writer.open_internal(fixture.path, FileAccess::WRITE) == OK);
		REQUIRE(writer.store_buffer(plain.ptr(), plain.size()));
		const size_t allocation = TestAllocation::allocation_size([&]() {
			if (table) {
				Vector<uint32_t> sizes;
				REQUIRE(sizes.resize(plain.size() / 32 + 1) == OK);
			} else {
				Vector<uint8_t> compressed;
				REQUIRE(compressed.resize(Compression::get_max_compressed_buffer_size(32, Compression::MODE_GZIP)) == OK);
			}
		});
		QuietErrors quiet;
		{
			TestAllocation::Failure failure(allocation);
			writer.close();
			CHECK(TestAllocation::failures() == 1);
		}
		CHECK_FALSE(writer.is_open());
		CHECK(writer.get_error() == ERR_OUT_OF_MEMORY);
		writer.close();
		CHECK(writer.get_error() == ERR_OUT_OF_MEMORY);
	}
}
#endif
} //namespace TestCompressedWriterSafety
