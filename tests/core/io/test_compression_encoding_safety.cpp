/**************************************************************************/
/*  test_image_loader_staging.cpp                                         */
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
TEST_FORCE_LINK(test_compression_encoding_safety)

#include "core/io/compression.h"
#include "core/variant/variant.h"
#include "tests/core/io/allocation_test_helpers.h"

#include <cstring>

namespace TestCompressionEncodingSafety {
struct QuietErrors {
	bool previous = CoreGlobals::print_error_enabled;
	QuietErrors() { CoreGlobals::print_error_enabled = false; }
	~QuietErrors() { CoreGlobals::print_error_enabled = previous; }
};
static Vector<uint8_t> payload() {
	Vector<uint8_t> bytes;
	REQUIRE(bytes.resize(513) == OK);
	for (int i = 0; i < bytes.size(); i++) {
		bytes.write[i] = i % 251;
	}
	return bytes;
}
TEST_CASE("[N039][CompressionEncoding] Public encoding preserves supported successful round trips") {
	for (Compression::Mode mode : { Compression::MODE_FASTLZ, Compression::MODE_GZIP, Compression::MODE_LZMA2 }) {
		const Vector<uint8_t> plain = payload();
		Variant source = plain;
		Callable::CallError error;
		Variant argument = int(mode);
		const Variant *arguments[] = { &argument };
		const Vector<uint8_t> encoded = source.callp("compress", arguments, 1, error);
		REQUIRE(error.error == Callable::CallError::CALL_OK);
		REQUIRE_FALSE(encoded.is_empty());
		Vector<uint8_t> decoded;
		REQUIRE(decoded.resize(plain.size()) == OK);
		CHECK(Compression::decompress(decoded.ptrw(), decoded.size(), encoded.ptr(), encoded.size(), mode) == plain.size());
		CHECK(decoded == plain);
	}
}
TEST_CASE("[N039][CompressionEncoding] Missing nonempty destinations fail before codec or copy") {
	const Vector<uint8_t> plain = payload();
	QuietErrors quiet;
	for (Compression::Mode mode : { Compression::MODE_FASTLZ, Compression::MODE_GZIP, Compression::MODE_LZMA2 }) {
		CHECK(Compression::compress(nullptr, plain.ptr(), plain.size(), mode) == -1);
	}
	CHECK(Compression::compress(nullptr, nullptr, 0, Compression::MODE_LZMA2) == 0);
}
#ifdef B01_ALLOCATION_TESTS
TEST_CASE("[N039][CompressionEncodingAllocation] Failed public output allocation returns empty without encoding") {
	const Vector<uint8_t> plain = payload();
	const int64_t bound = Compression::get_max_compressed_buffer_size(plain.size(), Compression::MODE_FASTLZ);
	const size_t allocation = TestAllocation::allocation_size([&]() {
		Vector<uint8_t> output;
		REQUIRE(output.resize(bound) == OK);
	});
	Variant source = plain;
	Variant mode = int(Compression::MODE_FASTLZ);
	const Variant *arguments[] = { &mode };
	Callable::CallError error;
	REQUIRE_FALSE(Vector<uint8_t>(source.callp("compress", arguments, 1, error)).is_empty());
	QuietErrors quiet;
	{
		TestAllocation::Failure failure(allocation);
		const Vector<uint8_t> output = source.callp("compress", arguments, 1, error);
		CHECK(output.is_empty());
		CHECK(error.error == Callable::CallError::CALL_OK);
		CHECK(TestAllocation::failures() == 1);
	}
	REQUIRE_FALSE(Vector<uint8_t>(source.callp("compress", arguments, 1, error)).is_empty());
}
TEST_CASE("[N039][CompressionEncodingAllocation] Failed LZMA2 input staging preserves the output buffer") {
	const Vector<uint8_t> plain = payload();
	Vector<uint8_t> encoded;
	REQUIRE(encoded.resize(Compression::get_max_compressed_buffer_size(plain.size(), Compression::MODE_LZMA2)) == OK);
	memset(encoded.ptrw(), 0xb6, encoded.size());
	const size_t allocation = TestAllocation::allocation_size([&]() {
		Vector<uint8_t> copy;
		REQUIRE(copy.resize(plain.size()) == OK);
	});
	QuietErrors quiet;
	{
		TestAllocation::Failure failure(allocation);
		CHECK(Compression::compress(encoded.ptrw(), plain.ptr(), plain.size(), Compression::MODE_LZMA2) == -1);
		CHECK(TestAllocation::failures() == 1);
	}
	for (uint8_t byte : encoded) {
		CHECK(byte == 0xb6);
	}
	CHECK(Compression::compress(encoded.ptrw(), plain.ptr(), plain.size(), Compression::MODE_LZMA2) > 0);
}
#endif
} // namespace TestCompressionEncodingSafety
