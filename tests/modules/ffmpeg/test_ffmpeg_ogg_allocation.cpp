/**************************************************************************/
/*  test_ffmpeg_ogg_allocation.cpp                                        */
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

TEST_FORCE_LINK(test_ffmpeg_ogg_allocation)

#include "modules/modules_enabled.gen.h"
#ifdef MODULE_FFMPEG_ENABLED
#include "tests/core/io/allocation_test_helpers.h"
#include "tests/test_utils.h"

#include "modules/ffmpeg/ffmpeg_ogg_helpers.h"

#ifdef MODULE_VORBIS_ENABLED
#include "servers/audio/audio_server.h"

#include "modules/vorbis/audio_stream_ogg_vorbis.h"
#endif

namespace TestFFmpegOggAllocation {
TEST_CASE("[N016][FFmpegOgg] Cumulative append extents preserve64-bit lengths and reject overflow") {
	int64_t size = 0;
	CHECK(ffmpeg_ogg_append_size(int64_t(1) << 32, 27, 65535, size) == OK);
	CHECK(size == (int64_t(1) << 32) + 27 + 65535);
	CHECK(ffmpeg_ogg_append_size(INT64_MAX - 2, 1, 1, size) == OK);
	CHECK(size == INT64_MAX);
	CHECK(ffmpeg_ogg_append_size(INT64_MAX - 2, 2, 1, size) == ERR_INVALID_DATA);
	CHECK(ffmpeg_ogg_append_size(INT64_MAX, 1, 0, size) == ERR_INVALID_DATA);
	CHECK(ffmpeg_ogg_append_size(-1, 1, 1, size) == ERR_INVALID_DATA);
	CHECK(ffmpeg_ogg_append_size(0, -1, 1, size) == ERR_INVALID_DATA);
	CHECK(ffmpeg_ogg_append_size(0, 1, -1, size) == ERR_INVALID_DATA);
}

TEST_CASE("[N016][FFmpegOgg] Header body and repeated appends preserve bytes and shared prefixes") {
	uint8_t header[] = { 1, 2, 3 };
	uint8_t body[] = { 4, 5, 6, 7 };
	ogg_page page = {};
	page.header = header;
	page.header_len = sizeof(header);
	page.body = body;
	page.body_len = sizeof(body);
	Vector<uint8_t> data;
	CHECK(ffmpeg_append_ogg_page(data, page) == OK);
	const Vector<uint8_t> first = data;
	CHECK(ffmpeg_append_ogg_page(data, page) == OK);
	REQUIRE(data.size() == 14);
	REQUIRE(first.size() == 7);
	if (data.size() != 14 || first.size() != 7) {
		return;
	}
	for (int i = 0; i < 14; i++) {
		CHECK(data[i] == uint8_t(i % 7 + 1));
	}
	for (int i = 0; i < 7; i++) {
		CHECK(first[i] == uint8_t(i + 1));
	}
	const Vector<uint8_t> previous = data;
	page = {};
	CHECK(ffmpeg_append_ogg_page(data, page) == OK);
	CHECK(data == previous);
}

TEST_CASE("[N016][FFmpegOgg] Invalid page extents and absent nonempty planes preserve the destination") {
	Vector<uint8_t> data = String("prefix").to_utf8_buffer();
	const Vector<uint8_t> previous = data;
	ogg_page page = {};
	page.header_len = -1;
	CHECK(ffmpeg_append_ogg_page(data, page) == ERR_INVALID_DATA);
	CHECK(data == previous);
	page.header_len = 1;
	CHECK(ffmpeg_append_ogg_page(data, page) == ERR_INVALID_DATA);
	CHECK(data == previous);
	page.header_len = 0;
	page.body_len = 1;
	CHECK(ffmpeg_append_ogg_page(data, page) == ERR_INVALID_DATA);
	CHECK(data == previous);
}

#ifdef B01_ALLOCATION_TESTS
TEST_CASE("[N016][FFmpegOgg] Failed first and subsequent page growth return before copying") {
	uint8_t header[27] = {};
	uint8_t body[1024] = {};
	ogg_page page = {};
	page.header = header;
	page.header_len = sizeof(header);
	page.body = body;
	page.body_len = sizeof(body);
	for (bool nonempty : { false, true }) {
		Vector<uint8_t> data = nonempty ? String("retained prefix").to_utf8_buffer() : Vector<uint8_t>();
		const Vector<uint8_t> previous = data;
		const int64_t required_size = data.size() + sizeof(header) + sizeof(body);
		const size_t allocation = TestAllocation::allocation_size([&]() {
			Vector<uint8_t> probe = previous;
			CHECK(probe.resize(required_size) == OK);
		});
		{
			TestAllocation::Failure failure(allocation);
			CHECK(ffmpeg_append_ogg_page(data, page) == ERR_OUT_OF_MEMORY);
			CHECK(TestAllocation::failures() > 0);
		}
		CHECK(data == previous);
		CHECK(ffmpeg_append_ogg_page(data, page) == OK);
		CHECK(data.size() == required_size);
		for (int i = 0; i < previous.size(); i++) {
			CHECK(data[i] == previous[i]);
		}
	}
}
#endif

#ifdef MODULE_VORBIS_ENABLED
TEST_CASE("[Audio][N016][FFmpegOgg] Ordinary typed Vorbis reconstruction remains playable") {
	Ref<AudioStreamOggVorbis> stream = AudioStreamOggVorbis::load_from_file(TestUtils::get_data_path("media/ffmpeg_tone_44100.ogg"));
	REQUIRE(stream.is_valid());
	if (stream.is_null()) {
		return;
	}
	stream->set_loop(false);
	Ref<AudioStreamPlayback> playback = stream->instantiate_playback();
	REQUIRE(playback.is_valid());
	if (playback.is_null()) {
		return;
	}
	playback->start_playback();
	AudioFrame frames[256];
	int total = 0;
	float peak = 0.0f;
	for (int batch = 0; batch < 1000 && playback->is_playing(); batch++) {
		const int mixed = playback->mix(frames, 1.0f, 256);
		REQUIRE(mixed >= 0);
		REQUIRE(mixed <= 256);
		if (mixed < 0 || mixed > 256) {
			return;
		}
		total += mixed;
		for (int i = 0; i < mixed; i++) {
			peak = MAX(peak, MAX(Math::abs(frames[i].left), Math::abs(frames[i].right)));
		}
	}
	CHECK_FALSE(playback->is_playing());
	CHECK(total > 2000);
	CHECK(total < int(AudioServer::get_singleton()->get_mix_rate()) * 2);
	CHECK(peak > 0.01f);
	playback->stop_playback();
}
#endif
} // namespace TestFFmpegOggAllocation
#endif
