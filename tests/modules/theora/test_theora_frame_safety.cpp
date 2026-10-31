/**************************************************************************/
/*  test_theora_frame_safety.cpp                                  */
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

#include "modules/modules_enabled.gen.h"
TEST_FORCE_LINK(test_theora_frame_safety)

#ifdef MODULE_THEORA_ENABLED
#include "tests/core/io/allocation_test_helpers.h"

#include "modules/theora/theora_frame_helpers.h"

#include <theora/theoraenc.h>

namespace TestTheoraFrame {
static void check_pixels(const th_ycbcr_buffer p_planes, const Rect2i &p_crop, th_pixel_fmt p_format, const Vector<uint8_t> &p_output) {
	const int sx = p_format == TH_PF_444 ? 0 : 1;
	const int sy = p_format == TH_PF_420 ? 1 : 0;
	REQUIRE(p_output.size() == int64_t(p_crop.size.x) * p_crop.size.y * 4);
	for (int y = 0; y < p_crop.size.y; y++) {
		for (int x = 0; x < p_crop.size.x; x++) {
			const int ax = p_crop.position.x + x;
			const int ay = p_crop.position.y + y;
			const uint8_t yy = p_planes[0].data[ay * p_planes[0].stride + ax];
			const uint8_t uu = p_planes[1].data[(ay >> sy) * p_planes[1].stride + (ax >> sx)];
			const uint8_t vv = p_planes[2].data[(ay >> sy) * p_planes[2].stride + (ax >> sx)];
			// Independent per-pixel reference; retain the established table color math.
			uint32_t pixel = tables[yy] + tables[256 + uu] + tables[512 + vv];
			uint32_t flags = pixel & 0x40080100U;
			if (flags != 0) {
				flags -= flags >> 8;
				pixel |= flags;
				flags = 0x40080100U & ~(pixel >> 1);
				pixel += flags >> 8;
			}
			const int at = (y * p_crop.size.x + x) * 4;
			CHECK(p_output[at] == uint8_t(pixel >> 11));
			CHECK(p_output[at + 1] == uint8_t(pixel >> 22));
			CHECK(p_output[at + 2] == uint8_t(pixel));
			CHECK(p_output[at + 3] == 255);
		}
	}
}

struct Planes {
	Vector<uint8_t> data[3];
	th_ycbcr_buffer planes;
	explicit Planes(th_pixel_fmt p_format = TH_PF_420) {
		const int sx = p_format == TH_PF_444 ? 0 : 1;
		const int sy = p_format == TH_PF_420 ? 1 : 0;
		for (int i = 0; i < 3; i++) {
			const int width = i == 0 ? 16 : 16 >> sx;
			const int height = i == 0 ? 16 : 16 >> sy;
			const int stride = width + i + 3;
			REQUIRE(data[i].resize(stride * height) == OK);
			for (int y = 0; y < height; y++) {
				for (int x = 0; x < stride; x++) {
					data[i].write[y * stride + x] = uint8_t(16 + (y * 17 + x * 13 + i * 47) % 220);
				}
			}
			planes[i] = { width, height, stride, data[i].ptrw() };
		}
	}
};

TEST_CASE("[R4][TheoraFrame] Absolute crops preserve all chroma phases and independent strides") {
	for (th_pixel_fmt format : { TH_PF_420, TH_PF_422, TH_PF_444 }) {
		Planes planes(format);
		for (int x : { 0, 1, 2, 3 }) {
			for (int y : { 0, 1, 2, 3 }) {
				for (int width : { 1, 2, 3, 8 }) {
					for (int height : { 1, 2, 3, 8 }) {
						const Rect2i crop(x, y, width, height);
						Vector<uint8_t> output;
						REQUIRE(TheoraFrame::convert(planes.planes, crop, format, output) == OK);
						check_pixels(planes.planes, crop, format, output);
					}
				}
			}
		}
	}
}

TEST_CASE("[R4][TheoraFrame] Invalid geometry and planes leave the previous frame untouched") {
	Vector<uint8_t> output;
	REQUIRE(TheoraFrame::prepare_rgba(Vector2i(8, 8), output) == OK);
	CHECK(output.size() == 256);
	for (uint8_t byte : output) {
		CHECK(byte == 0);
	}
	output.fill(0x6b);
	const uint8_t *previous = output.ptr();
	for (const Vector2i &size : { Vector2i(32768, 16384), Vector2i(65536, 16384), Vector2i(0, 8), Vector2i(-1, 8), Vector2i(INT32_MAX, 1) }) {
		CHECK(TheoraFrame::prepare_rgba(size, output) == ERR_FILE_CORRUPT);
		CHECK(output.ptr() == previous);
	}
	Planes planes;
	for (const Rect2i &crop : { Rect2i(-1, 0, 8, 8), Rect2i(0, -1, 8, 8), Rect2i(0, 0, 0, 8), Rect2i(0, 0, 8, -1),
				 Rect2i(0, 0, 32768, 16384), Rect2i(0, 0, 65536, 16384), Rect2i(9, 9, 8, 8), Rect2i(INT32_MAX, INT32_MAX, 2, 2) }) {
		CHECK(TheoraFrame::convert(planes.planes, crop, TH_PF_420, output) == ERR_FILE_CORRUPT);
		CHECK(output.ptr() == previous);
	}
	CHECK(TheoraFrame::convert(planes.planes, Rect2i(0, 0, 8, 8), TH_PF_RSVD, output) == ERR_FILE_CORRUPT);
	for (int plane = 0; plane < 3; plane++) {
		const th_img_plane valid = planes.planes[plane];
		for (int bad = 0; bad < 5; bad++) {
			planes.planes[plane] = valid;
			switch (bad) {
				case 0:
					planes.planes[plane].data = nullptr;
					break;
				case 1:
					planes.planes[plane].width = 0;
					break;
				case 2:
					planes.planes[plane].height = 1;
					break;
				case 3:
					planes.planes[plane].stride = -valid.stride;
					break;
				case 4:
					planes.planes[plane].stride = valid.width - 1;
					break;
			}
			CHECK(TheoraFrame::convert(planes.planes, Rect2i(1, 1, 8, 8), TH_PF_420, output) == ERR_FILE_CORRUPT);
			CHECK(output.ptr() == previous);
		}
		planes.planes[plane] = valid;
	}
	for (uint8_t byte : output) {
		CHECK(byte == 0x6b);
	}
}

TEST_CASE("[R4][TheoraFrame] New frames preserve a retained Image without copy-on-write") {
	Planes planes;
	Vector<uint8_t> output;
	REQUIRE(TheoraFrame::convert(planes.planes, Rect2i(0, 0, 8, 8), TH_PF_420, output) == OK);
	const Vector<uint8_t> original = output;
	const Ref<Image> image = Image::create_from_data(8, 8, false, Image::FORMAT_RGBA8, output);
	REQUIRE(image.is_valid());
	REQUIRE_FALSE(image->is_empty());
	const uint8_t *previous = output.ptr();
	REQUIRE(TheoraFrame::convert(planes.planes, Rect2i(1, 1, 8, 8), TH_PF_420, output) == OK);
	CHECK(output.ptr() != previous);
	CHECK(image->get_data() == original);
	check_pixels(planes.planes, Rect2i(1, 1, 8, 8), TH_PF_420, output);
	REQUIRE(TheoraFrame::prepare_rgba(Vector2i(8, 8), output) == OK);
	CHECK(image->get_data() == original);
}

#ifdef THEORA_BUILTIN_TESTS_ENABLED
struct Codec {
	th_info info;
	th_info parsed;
	th_comment comment;
	th_comment parsed_comment;
	th_setup_info *setup = nullptr;
	th_enc_ctx *encoder = nullptr;
	th_dec_ctx *decoder = nullptr;
	Codec() {
		th_info_init(&info);
		th_info_init(&parsed);
		th_comment_init(&comment);
		th_comment_init(&parsed_comment);
	}
	~Codec() {
		th_decode_free(decoder);
		th_encode_free(encoder);
		th_setup_free(setup);
		th_info_clear(&info);
		th_info_clear(&parsed);
		th_comment_clear(&comment);
		th_comment_clear(&parsed_comment);
	}
};

TEST_CASE("[R4][TheoraFrame] Real pinned Theora decoder output preserves odd picture origins") {
	for (th_pixel_fmt format : { TH_PF_420, TH_PF_422, TH_PF_444 }) {
		for (const Vector2i &origin : { Vector2i(0, 0), Vector2i(0, 1), Vector2i(1, 1) }) {
			Codec codec;
			codec.info.frame_width = codec.info.frame_height = 16;
			codec.info.pic_width = codec.info.pic_height = 8;
			codec.info.pic_x = origin.x;
			codec.info.pic_y = origin.y;
			codec.info.fps_numerator = 30;
			codec.info.fps_denominator = 1;
			codec.info.aspect_numerator = codec.info.aspect_denominator = 1;
			codec.info.pixel_fmt = format;
			codec.info.quality = 63;
			codec.info.keyframe_granule_shift = 6;
			codec.encoder = th_encode_alloc(&codec.info);
			REQUIRE(codec.encoder != nullptr);
			ogg_packet packet = {};
			int headers = 0;
			while (th_encode_flushheader(codec.encoder, &codec.comment, &packet) > 0) {
				REQUIRE(th_decode_headerin(&codec.parsed, &codec.parsed_comment, &codec.setup, &packet) >= 0);
				headers++;
			}
			REQUIRE(headers == 3);
			codec.decoder = th_decode_alloc(&codec.parsed, codec.setup);
			REQUIRE(codec.decoder != nullptr);
			// Encode a tiny local frame using the pinned provider.
			Planes input(format);
			REQUIRE(th_encode_ycbcr_in(codec.encoder, input.planes) == 0);
			int frames = 0;
			while (th_encode_packetout(codec.encoder, 1, &packet) > 0) {
				ogg_int64_t granule = 0;
				REQUIRE(th_decode_packetin(codec.decoder, &packet, &granule) >= 0);
				frames++;
			}
			REQUIRE(frames == 1);
			th_ycbcr_buffer decoded;
			REQUIRE(th_decode_ycbcr_out(codec.decoder, decoded) == 0);
			CHECK(codec.parsed.pic_x == uint32_t(origin.x));
			CHECK(codec.parsed.pic_y == uint32_t(origin.y));
			const Rect2i crop(origin, Vector2i(8, 8));
			Vector<uint8_t> output;
			REQUIRE(TheoraFrame::convert(decoded, crop, format, output) == OK);
			check_pixels(decoded, crop, format, output);
		}
	}
}

#endif // THEORA_BUILTIN_TESTS_ENABLED

#ifdef B01_ALLOCATION_TESTS
TEST_CASE("[R4][TheoraFrame] Allocation failure preserves shared image data and permits retry") {
	Planes planes;
	Vector<uint8_t> output;
	REQUIRE(TheoraFrame::convert(planes.planes, Rect2i(0, 0, 8, 8), TH_PF_420, output) == OK);
	const Vector<uint8_t> original = output;
	const Ref<Image> image = Image::create_from_data(8, 8, false, Image::FORMAT_RGBA8, output);
	REQUIRE(image.is_valid());
	const uint8_t *previous = output.ptr();
	const size_t exact = TestAllocation::allocation_size([] {
		Vector<uint8_t> probe;
		REQUIRE(probe.resize(256) == OK);
	});
	REQUIRE(exact != 0);
	for (bool prepare : { false, true }) {
		Error error;
		ERR_PRINT_OFF;
		{
			TestAllocation::Failure fault(exact);
			error = prepare ? TheoraFrame::prepare_rgba(Vector2i(8, 8), output) : TheoraFrame::convert(planes.planes, Rect2i(1, 1, 8, 8), TH_PF_420, output);
		}
		ERR_PRINT_ON;
		CHECK(TestAllocation::failures() == 1);
		CHECK(error == ERR_OUT_OF_MEMORY);
		CHECK(output.ptr() == previous);
		CHECK(output == original);
		CHECK(image->get_data() == original);
	}
	REQUIRE(TheoraFrame::convert(planes.planes, Rect2i(1, 1, 8, 8), TH_PF_420, output) == OK);
	check_pixels(planes.planes, Rect2i(1, 1, 8, 8), TH_PF_420, output);
	CHECK(image->get_data() == original);
}
#endif
} // namespace TestTheoraFrame
#endif
