/**************************************************************************/
/*  test_exr_tile_bounds.cpp                                              */
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
TEST_FORCE_LINK(test_exr_tile_bounds)
#include "modules/modules_enabled.gen.h"
#ifdef MODULE_TINYEXR_ENABLED
#include "thirdparty/tinyexr/tinyexr.h"

#include "core/io/marshalls.h"
#include "tests/core/io/exr_test_helpers.h"

#include "modules/tinyexr/exr_image_size.h"

#include <cstring>

namespace TestEXRTileBounds {
struct QuietErrors {
	bool previous = CoreGlobals::print_error_enabled;
	QuietErrors() { CoreGlobals::print_error_enabled = false; }
	~QuietErrors() { CoreGlobals::print_error_enabled = previous; }
};
static int header_end(const Vector<uint8_t> &p_bytes) {
	int position = 8;
	while (p_bytes[position] != 0) {
		while (p_bytes[position++] != 0) {
		}
		while (p_bytes[position++] != 0) {
		}
		const uint32_t length = decode_uint32(p_bytes.ptr() + position);
		position += 4 + length;
		REQUIRE(position < p_bytes.size());
	}
	return position + 1;
}
TEST_CASE("[N034][EXRTileBounds] Invalid tile coordinates reject before destination publication") {
	for (Vector2i coordinate : { Vector2i(-1, 0), Vector2i(0, -1), Vector2i(INT_MAX, 0), Vector2i(0, INT_MAX), Vector2i(9, 0), Vector2i(0, 10) }) {
		Vector<uint8_t> encoded = TestEXRFixture::fixture("R", false, true);
		const uint64_t first_chunk = decode_uint64(encoded.ptr() + header_end(encoded));
		REQUIRE(first_chunk + 20 < uint64_t(encoded.size()));
		encode_uint32(uint32_t(coordinate.x), encoded.ptrw() + first_chunk);
		encode_uint32(uint32_t(coordinate.y), encoded.ptrw() + first_chunk + 4);
		Ref<Image> image = Image::create_empty(2, 3, false, Image::FORMAT_RGBA8);
		image->fill(Color(0.25, 0.5, 0.75));
		const Vector<uint8_t> before = image->get_data();
		QuietErrors quiet;
		CHECK(TestEXRFixture::load(encoded, image) == ERR_FILE_CORRUPT);
		CHECK(image->get_size() == Vector2i(2, 3));
		CHECK(image->get_data() == before);
	}
}
TEST_CASE("[N034][EXRTileBounds] Legal edge and oversized tiles retain FLOAT and HALF pixels") {
	for (int tile_size : { 2, 32 }) {
		for (bool half : { false, true }) {
			Ref<Image> image;
			image.instantiate();
			REQUIRE(TestEXRFixture::load(TestEXRFixture::fixture("R,G,B,A", half, true, false, "", 1, tile_size), image) == OK);
			CHECK(image->get_size() == Vector2i(17, 19));
			CHECK(image->get_pixel(16, 18).r == doctest::Approx(0.125f + 322.0f / 64.0f));
		}
	}
}
TEST_CASE("[N034][EXRTileBounds] Defensive destination geometry checks wide origins and full rectangles") {
	int64_t x, y;
	CHECK(EXRImageSize::tile_rectangle(17, 19, 2, 2, 8, 9, 1, 1, x, y) == OK);
	CHECK(x == 16);
	CHECK(y == 18);
	CHECK(EXRImageSize::tile_rectangle(17, 19, 32, 32, 0, 0, 17, 19, x, y) == OK);
	for (Vector2i coordinate : { Vector2i(-1, 0), Vector2i(INT_MAX, INT_MAX) }) {
		CHECK(EXRImageSize::tile_rectangle(17, 19, INT_MAX, INT_MAX, coordinate.x, coordinate.y, 1, 1, x, y) == ERR_FILE_CORRUPT);
	}
	CHECK(EXRImageSize::tile_rectangle(17, 19, 2, 2, 8, 9, 2, 2, x, y) == ERR_FILE_CORRUPT);
	CHECK(EXRImageSize::tile_rectangle(17, 19, 0, 2, 0, 0, 1, 1, x, y) == ERR_FILE_CORRUPT);
}
static int tile_descriptor(const Vector<uint8_t> &p_bytes) {
	const char marker[] = "tiles\0tiledesc\0";
	constexpr int marker_size = sizeof(marker) - 1;
	for (int i = 8; i + marker_size + 4 + 9 <= p_bytes.size(); i++) {
		if (memcmp(p_bytes.ptr() + i, marker, marker_size) == 0) {
			REQUIRE(decode_uint32(p_bytes.ptr() + i + marker_size) == 9);
			return i + marker_size + 4;
		}
	}
	CHECK(false);
	return -1;
}
TEST_CASE("[N034][EXRTileBounds] Zero descriptor extents reject through the actual image loader") {
	for (bool half : { false, true }) {
		const Vector<uint8_t> original = TestEXRFixture::fixture("R,G,B,A", half, true);
		const int descriptor = tile_descriptor(original);
		REQUIRE(descriptor >= 0);
		if (descriptor < 0) {
			return;
		}
		for (int zero_mask : { 1, 2, 3 }) {
			Vector<uint8_t> encoded = original;
			if (zero_mask & 1) {
				encode_uint32(0, encoded.ptrw() + descriptor);
			}
			if (zero_mask & 2) {
				encode_uint32(0, encoded.ptrw() + descriptor + 4);
			}
			Ref<Image> image = Image::create_empty(2, 3, false, Image::FORMAT_RGBA8);
			image->fill(Color(0.25, 0.5, 0.75));
			const Vector<uint8_t> before = image->get_data();
			QuietErrors quiet;
			CHECK(TestEXRFixture::load(encoded, image) == ERR_FILE_CORRUPT);
			CHECK(image->get_size() == Vector2i(2, 3));
			CHECK(image->get_data() == before);
			REQUIRE(TestEXRFixture::load(original, image) == OK);
			CHECK(image->get_size() == Vector2i(17, 19));
		}
	}
}
TEST_CASE("[N034][EXRTileBounds] Constructed dependency headers reject nonpositive extents before tile counting") {
	const Vector<uint8_t> encoded = TestEXRFixture::fixture("R", false, true);
	EXRVersion version = {};
	const int version_result = ParseEXRVersionFromMemory(&version, encoded.ptr(), encoded.size());
	REQUIRE(version_result == TINYEXR_SUCCESS);
	if (version_result != TINYEXR_SUCCESS) {
		return;
	}
	EXRHeader header;
	InitEXRHeader(&header);
	struct HeaderCleanup {
		EXRHeader *header;
		~HeaderCleanup() { FreeEXRHeader(header); }
	} cleanup = { &header };
	const char *message = nullptr;
	const int parsed = ParseEXRHeaderFromMemory(&header, &version, encoded.ptr(), encoded.size(), &message);
	if (message) {
		FreeEXRErrorMessage(message);
	}
	REQUIRE(parsed == TINYEXR_SUCCESS);
	if (parsed != TINYEXR_SUCCESS) {
		return;
	}
	const int original_x = header.tile_size_x;
	const int original_y = header.tile_size_y;
	for (int axis : { 0, 1 }) {
		for (int extent : { 0, -1 }) {
			header.tile_size_x = axis == 0 ? extent : original_x;
			header.tile_size_y = axis == 1 ? extent : original_y;
			EXRImage decoded;
			InitEXRImage(&decoded);
			message = nullptr;
			CHECK(LoadEXRImageFromMemory(&decoded, &header, encoded.ptr(), encoded.size(), &message) == TINYEXR_ERROR_INVALID_DATA);
			CHECK(decoded.images == nullptr);
			CHECK(decoded.tiles == nullptr);
			FreeEXRImage(&decoded);
			if (message) {
				FreeEXRErrorMessage(message);
			}
		}
	}
	header.tile_size_x = original_x;
	header.tile_size_y = original_y;
	EXRImage valid;
	InitEXRImage(&valid);
	message = nullptr;
	CHECK(LoadEXRImageFromMemory(&valid, &header, encoded.ptr(), encoded.size(), &message) == TINYEXR_SUCCESS);
	FreeEXRImage(&valid);
	if (message) {
		FreeEXRErrorMessage(message);
	}
}
} //namespace TestEXRTileBounds
#endif
