/**************************************************************************/
/*  test_yuv_traversal.cpp                                  */
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
TEST_FORCE_LINK(test_yuv_traversal)
#include "thirdparty/misc/yuv2rgb.h"

#include "core/templates/vector.h"
namespace TestYuvTraversal {
// This reference preserves the established table-based color calculation while
// selecting each source pixel independently of the converter's traversal.
static uint32_t reference_pixel(uint8_t p_y, uint8_t p_u, uint8_t p_v) {
	uint32_t pixel = tables[p_y] + tables[256 + p_u] + tables[512 + p_v];
	uint32_t flags = pixel & 0x40080100U;
	if (flags != 0) {
		flags -= flags >> 8;
		pixel |= flags;
		flags = 0x40080100U & ~(pixel >> 1);
		pixel += flags >> 8;
	}
	return pixel;
}
using Converter = void (*)(uint8_t *, const uint8_t *, const uint8_t *, const uint8_t *, int32_t, int32_t, int32_t, int32_t, int32_t);
static void check(int p_width, int p_height, int p_shift_x, int p_shift_y, Converter p_convert, bool p_reversed = false) {
	const int chroma_width = (p_width + (1 << p_shift_x) - 1) >> p_shift_x;
	const int chroma_height = (p_height + (1 << p_shift_y) - 1) >> p_shift_y;
	const int y_span = p_width + 3;
	const int uv_span = chroma_width + 3;
	const int dst_span = p_width * 4 + 7;
	Vector<uint8_t> y, u, v, output;
	REQUIRE(y.resize(y_span * p_height) == OK);
	REQUIRE(u.resize(uv_span * chroma_height) == OK);
	REQUIRE(v.resize(uv_span * chroma_height) == OK);
	REQUIRE(output.resize(dst_span * p_height + 32) == OK);
	y.fill(0xcc);
	u.fill(0xcc);
	v.fill(0xcc);
	output.fill(0xcd);
	for (int row = 0; row < p_height; ++row) {
		for (int column = 0; column < p_width; ++column) {
			y.write[row * y_span + column] = uint8_t(16 + (row * 17 + column * 13) % 220);
		}
	}
	for (int row = 0; row < chroma_height; ++row) {
		for (int column = 0; column < chroma_width; ++column) {
			u.write[row * uv_span + column] = uint8_t(80 + (row * 13 + column * 17) % 96);
			v.write[row * uv_span + column] = uint8_t(90 + (row * 19 + column * 11) % 96);
		}
	}
	const uint8_t *y_start = y.ptr() + (p_reversed ? (p_height - 1) * y_span : 0);
	const uint8_t *u_start = u.ptr() + (p_reversed ? (chroma_height - 1) * uv_span : 0);
	const uint8_t *v_start = v.ptr() + (p_reversed ? (chroma_height - 1) * uv_span : 0);
	uint8_t *dst_start = output.ptrw() + 16 + (p_reversed ? (p_height - 1) * dst_span : 0);
	const int direction = p_reversed ? -1 : 1;
	p_convert(dst_start, y_start, u_start, v_start, p_width, p_height, direction * y_span, direction * uv_span, direction * dst_span);
	for (int row = 0; row < p_height; ++row) {
		for (int column = 0; column < p_width; ++column) {
			const uint8_t yy = y_start[int64_t(direction) * row * y_span + column];
			const uint8_t uu = u_start[int64_t(direction) * (row >> p_shift_y) * uv_span + (column >> p_shift_x)];
			const uint8_t vv = v_start[int64_t(direction) * (row >> p_shift_y) * uv_span + (column >> p_shift_x)];
			const uint32_t packed = reference_pixel(yy, uu, vv);
			const uint8_t *actual = dst_start + int64_t(direction) * row * dst_span + column * 4;
			CHECK(actual[0] == uint8_t(packed >> 11));
			CHECK(actual[1] == uint8_t(packed >> 22));
			CHECK(actual[2] == uint8_t(packed));
			CHECK(actual[3] == 255);
		}
		for (int pad = p_width * 4; pad < dst_span; ++pad) {
			CHECK(output[16 + row * dst_span + pad] == 0xcd);
		}
	}
	for (int guard = 0; guard < 16; ++guard) {
		CHECK(output[guard] == 0xcd);
		CHECK(output[output.size() - 16 + guard] == 0xcd);
	}
}
TEST_CASE("[R4][YUV] All chroma layouts preserve odd edges final rows strides and pixel order") {
	for (int width : { 1, 2, 3, 6, 7 }) {
		for (int height : { 1, 2, 3, 4, 7 }) {
			for (bool reversed : { false, true }) {
				check(width, height, 1, 1, yuv420_2_rgb8888, reversed);
				check(width, height, 1, 0, yuv422_2_rgb8888, reversed);
				check(width, height, 0, 0, yuv444_2_rgb8888, reversed);
			}
		}
	}
}
TEST_CASE("[R4][YUV] Thin images cross former 16-bit counter boundaries") {
	for (int size : { 32767, 32768, 65535, 65536, 65537 }) {
		check(size, 1, 1, 1, yuv420_2_rgb8888);
		check(1, size, 1, 1, yuv420_2_rgb8888);
		check(size, 1, 1, 0, yuv422_2_rgb8888);
		check(1, size, 1, 0, yuv422_2_rgb8888);
		check(size, 1, 0, 0, yuv444_2_rgb8888);
		check(1, size, 0, 0, yuv444_2_rgb8888);
	}
}
} // namespace TestYuvTraversal
