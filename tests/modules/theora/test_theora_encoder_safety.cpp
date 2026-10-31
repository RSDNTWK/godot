/**************************************************************************/
/*  test_theora_encoder_safety.cpp                                  */
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
TEST_FORCE_LINK(test_theora_encoder_safety)

#if defined(MODULE_THEORA_ENABLED) && defined(THEORA_BUILTIN_TESTS_ENABLED)
#include "modules/theora/theora_frame_helpers.h"
extern "C" {
#include "thirdparty/libtheora/encint.h"
}

namespace TestTheoraEncoder {
struct Codec {
	th_info info;
	th_info parsed;
	th_comment comment;
	th_comment decoded_comment;
	th_setup_info *setup = nullptr;
	th_enc_ctx *encoder = nullptr;
	th_dec_ctx *decoder = nullptr;
	Codec() {
		th_info_init(&info);
		th_info_init(&parsed);
		th_comment_init(&comment);
		th_comment_init(&decoded_comment);
	}
	~Codec() {
		th_decode_free(decoder);
		th_encode_free(encoder);
		th_setup_free(setup);
		th_info_clear(&info);
		th_info_clear(&parsed);
		th_comment_clear(&comment);
		th_comment_clear(&decoded_comment);
	}
};

TEST_CASE("[R4][TheoraEncoder] Portable C quantizer handles the complete signed coefficient range") {
	ogg_int16_t input[64] = {};
	ogg_int16_t output[64] = {};
	ogg_uint16_t dequant[64];
	oc_iquant table[64];
	// Power-of-two steps give an independent exact division reference.
	for (int step : { 16, 32, 64, 256, 1024 }) {
		for (int i = 0; i < 64; i++) {
			dequant[i] = step;
		}
		oc_enc_enquant_table_init_c(table, dequant);
		for (int value = INT16_MIN; value <= INT16_MAX; value++) {
			input[63] = value;
			const int magnitude = ((value < 0 ? -value : value) + step / 2) / step;
			const int expected = value < 0 ? -magnitude : magnitude;
			const int last = oc_enc_quantize_c(output, input, dequant, table);
			CHECK(last == (expected == 0 ? 0 : 63));
			CHECK(output[0] == 0);
			CHECK(output[63] == expected);
		}
	}
}

TEST_CASE("[R4][TheoraEncoder] Intra inter motion and trellis paths round-trip bounded local frames") {
	int cases = 0;
	for (int size : { 16, 32, 64 }) {
		for (th_pixel_fmt format : { TH_PF_420, TH_PF_422, TH_PF_444 }) {
			for (int quality : { 0, 31, 63 }) {
				for (bool fast : { false, true }) {
					for (int pattern : { 0, 1, 2, 3 }) {
						Codec codec;
						codec.info.frame_width = codec.info.frame_height = size;
						codec.info.pic_width = codec.info.pic_height = size - 8;
						codec.info.pic_x = pattern == 3;
						codec.info.pic_y = pattern >= 2;
						codec.info.fps_numerator = 30;
						codec.info.fps_denominator = 1;
						codec.info.aspect_numerator = codec.info.aspect_denominator = 1;
						codec.info.pixel_fmt = format;
						codec.info.quality = quality;
						codec.info.keyframe_granule_shift = 6;
						codec.encoder = th_encode_alloc(&codec.info);
						REQUIRE(codec.encoder != nullptr);
						int speed = 0;
						REQUIRE(th_encode_ctl(codec.encoder, TH_ENCCTL_GET_SPLEVEL_MAX, &speed, sizeof(speed)) == 0);
						if (!fast) {
							speed = 0;
						}
						REQUIRE(th_encode_ctl(codec.encoder, TH_ENCCTL_SET_SPLEVEL, &speed, sizeof(speed)) == 0);
						ogg_packet packet = {};
						int headers = 0;
						while (th_encode_flushheader(codec.encoder, &codec.comment, &packet) > 0) {
							REQUIRE(th_decode_headerin(&codec.parsed, &codec.decoded_comment, &codec.setup, &packet) >= 0);
							headers++;
						}
						REQUIRE(headers == 3);
						codec.decoder = th_decode_alloc(&codec.parsed, codec.setup);
						REQUIRE(codec.decoder != nullptr);
						Vector<uint8_t> input[3];
						th_ycbcr_buffer planes;
						const int sx = format == TH_PF_444 ? 0 : 1;
						const int sy = format == TH_PF_420 ? 1 : 0;
						for (int i = 0; i < 3; i++) {
							const int width = i == 0 ? size : size >> sx;
							const int height = i == 0 ? size : size >> sy;
							REQUIRE(input[i].resize(width * height) == OK);
							planes[i] = { width, height, width, input[i].ptrw() };
						}
						uint32_t random = 0x12345678U + uint32_t(cases);
						int frames = 0;
						for (int frame = 0; frame < 2; frame++) {
							for (int i = 0; i < 3; i++) {
								for (int y = 0; y < planes[i].height; y++) {
									for (int x = 0; x < planes[i].width; x++) {
										random = random * 1664525U + 1013904223U;
										int value = 128;
										if (pattern == 1) {
											value = (x * 17 + y * 3 + frame * 11 + i * 47) & 255;
										} else if (pattern == 2) {
											value = ((x + y + frame) & 1) ? 240 : 16;
										} else if (pattern == 3) {
											value = int(random >> 24);
										}
										input[i].write[y * planes[i].stride + x] = uint8_t(value);
									}
								}
							}
							REQUIRE(th_encode_ycbcr_in(codec.encoder, planes) == 0);
							while (th_encode_packetout(codec.encoder, frame == 1, &packet) > 0) {
								ogg_int64_t granule = 0;
								REQUIRE(th_decode_packetin(codec.decoder, &packet, &granule) >= 0);
								th_ycbcr_buffer decoded;
								REQUIRE(th_decode_ycbcr_out(codec.decoder, decoded) == 0);
								Vector<uint8_t> rgba;
								REQUIRE(TheoraFrame::convert(decoded, Rect2i(codec.parsed.pic_x, codec.parsed.pic_y, size - 8, size - 8), format, rgba) == OK);
								CHECK(rgba.size() == (size - 8) * (size - 8) * 4);
								frames++;
							}
						}
						CHECK(frames == 2);
						cases++;
					}
				}
			}
		}
	}
	CHECK(cases == 216);
}
} // namespace TestTheoraEncoder
#endif
