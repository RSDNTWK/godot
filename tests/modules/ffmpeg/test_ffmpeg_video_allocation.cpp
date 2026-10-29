/**************************************************************************/
/*  test_ffmpeg_video_allocation.cpp                                      */
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

TEST_FORCE_LINK(test_ffmpeg_video_allocation)

#include "modules/modules_enabled.gen.h"
#ifdef MODULE_FFMPEG_ENABLED
#include "core/os/os.h"
#include "scene/resources/image_texture.h"
#include "servers/audio/audio_server.h"
#include "tests/core/io/allocation_test_helpers.h"
#include "tests/test_utils.h"

#include "modules/ffmpeg/ffmpeg_video_frame_converter.h"
#include "modules/ffmpeg/video_stream_media.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <cstring>

namespace TestFFmpegVideoAllocation {
struct Frame {
	AVFrame *value = nullptr;
	Frame(int p_width, int p_height, AVPixelFormat p_format) {
		value = av_frame_alloc();
		if (!value) {
			return;
		}
		value->width = p_width;
		value->height = p_height;
		value->format = p_format;
		if (av_frame_get_buffer(value, 32) < 0) {
			av_frame_free(&value);
		}
	}
	~Frame() { av_frame_free(&value); }
	Frame(const Frame &) = delete;
	Frame &operator=(const Frame &) = delete;
};

static void fill_rgb(AVFrame *p_frame) {
	const int channels = p_frame->format == AV_PIX_FMT_RGBA ? 4 : 3;
	for (int y = 0; y < p_frame->height; y++) {
		uint8_t *row = p_frame->data[0] + y * p_frame->linesize[0];
		for (int x = 0; x < p_frame->width; x++) {
			row[x * channels] = uint8_t(x + y);
			row[x * channels + 1] = 42;
			row[x * channels + 2] = 81;
			if (channels == 4) {
				row[x * channels + 3] = 255;
			}
		}
	}
}

TEST_CASE("[F07][N012][FFmpegVideo] Geometry is checked before extent multiplication or conversion") {
	int64_t size = 0;
	CHECK(FFmpegVideoFrameConverter::get_rgba_size(16384, 16384, size) == OK);
	CHECK(size == int64_t(16384) * 16384 * 4);
	for (const Size2i dimensions : { Size2i(0, 1), Size2i(1, 0), Size2i(-1, 1),
				 Size2i(Image::MAX_WIDTH + 1, 1), Size2i(1, Image::MAX_HEIGHT + 1), Size2i(65536, 65536) }) {
		size = 123;
		CHECK(FFmpegVideoFrameConverter::get_rgba_size(dimensions.x, dimensions.y, size) == ERR_INVALID_DATA);
		CHECK(size == 0);
	}
	FFmpegVideoFrameConverter converter;
	Ref<Image> image = Image::create_empty(2, 3, false, Image::FORMAT_RGBA8);
	const Ref<Image> previous = image;
	AVFrame invalid = {};
	invalid.width = 65536;
	invalid.height = 65536;
	invalid.format = AV_PIX_FMT_RGBA;
	CHECK(converter.convert(&invalid, image) == ERR_INVALID_DATA);
	CHECK(image == previous);
	invalid.width = 2;
	invalid.height = 3;
	invalid.format = AV_PIX_FMT_NONE;
	CHECK(converter.convert(&invalid, image) == ERR_INVALID_DATA);
	CHECK(image == previous);
	CHECK(converter.convert(nullptr, image) == ERR_INVALID_PARAMETER);
	CHECK(image == previous);
}

TEST_CASE("[F07][N012][FFmpegVideo] Packed RGB and RGBA preserve padding and negative strides") {
	FFmpegVideoFrameConverter converter;
	for (AVPixelFormat format : { AV_PIX_FMT_RGB24, AV_PIX_FMT_RGBA }) {
		Frame frame(17, 19, format);
		REQUIRE(frame.value != nullptr);
		if (!frame.value) {
			return;
		}
		fill_rgb(frame.value);
		const int stride = frame.value->linesize[0];
		for (bool flipped : { false, true }) {
			if (flipped) {
				frame.value->data[0] += (frame.value->height - 1) * stride;
				frame.value->linesize[0] = -stride;
			}
			Ref<Image> image;
			REQUIRE(converter.convert(frame.value, image) == OK);
			REQUIRE(image.is_valid());
			if (image.is_null()) {
				return;
			}
			CHECK(image->get_width() == 17);
			CHECK(image->get_height() == 19);
			const Vector<uint8_t> pixels = image->get_data();
			REQUIRE(pixels.size() == 17 * 19 * 4);
			if (pixels.size() != 17 * 19 * 4) {
				return;
			}
			for (int y = 0; y < 19; y++) {
				for (int x = 0; x < 17; x++) {
					const int offset = (y * 17 + x) * 4;
					CHECK(pixels[offset] == uint8_t(x + (flipped ? 18 - y : y)));
					CHECK(pixels[offset + 1] == 42);
					CHECK(pixels[offset + 2] == 81);
					CHECK(pixels[offset + 3] == 255);
				}
			}
		}
	}
}

TEST_CASE("[F07][N012][FFmpegVideo] Planar conversion and cached geometry changes remain usable") {
	FFmpegVideoFrameConverter converter;
	for (const Size2i dimensions : { Size2i(32, 24), Size2i(64, 36), Size2i(32, 24) }) {
		Frame frame(dimensions.x, dimensions.y, AV_PIX_FMT_YUV420P);
		REQUIRE(frame.value != nullptr);
		if (!frame.value) {
			return;
		}
		for (int y = 0; y < dimensions.y; y++) {
			memset(frame.value->data[0] + y * frame.value->linesize[0], 82, dimensions.x);
		}
		for (int y = 0; y < dimensions.y / 2; y++) {
			memset(frame.value->data[1] + y * frame.value->linesize[1], 90, dimensions.x / 2);
			memset(frame.value->data[2] + y * frame.value->linesize[2], 240, dimensions.x / 2);
		}
		Ref<Image> image;
		REQUIRE(converter.convert(frame.value, image) == OK);
		REQUIRE(image.is_valid());
		if (image.is_null()) {
			return;
		}
		CHECK(image->get_size() == dimensions);
		const Color color = image->get_pixel(0, 0);
		CHECK(color.r > 0.9f);
		CHECK(color.g < 0.1f);
		CHECK(color.b < 0.1f);
		CHECK(color.a == 1.0f);
	}
	converter.clear();
	converter.clear();
}

TEST_CASE("[SceneTree][F07][FFmpegVideo] Initial and changed dimensions preserve the retained texture and RID") {
	Ref<ImageTexture> texture;
	texture.instantiate();
	const Ref<ImageTexture> retained = texture;
	const RID rid = texture->get_rid();
	FFmpegVideoFrameConverter converter;
	for (const Size2i dimensions : { Size2i(17, 19), Size2i(17, 19), Size2i(32, 24) }) {
		Frame frame(dimensions.x, dimensions.y, AV_PIX_FMT_RGBA);
		REQUIRE(frame.value != nullptr);
		if (!frame.value) {
			return;
		}
		fill_rgb(frame.value);
		Ref<Image> image;
		REQUIRE(converter.convert(frame.value, image) == OK);
		CHECK(FFmpegVideoFrameConverter::update_texture(image, texture) == OK);
		CHECK(texture == retained);
		CHECK(texture->get_rid() == rid);
		CHECK(texture->get_width() == dimensions.x);
		CHECK(texture->get_height() == dimensions.y);
	}
	CHECK(FFmpegVideoFrameConverter::update_texture(Ref<Image>(), texture) == ERR_INVALID_PARAMETER);
	CHECK(texture == retained);
	CHECK(texture->get_width() == 32);
}

#ifdef B01_ALLOCATION_TESTS
TEST_CASE("[F07][FFmpegVideo] Destination allocation failure preserves the published image and permits reuse") {
	FFmpegVideoFrameConverter converter;
	Frame frame(17, 19, AV_PIX_FMT_RGBA);
	REQUIRE(frame.value != nullptr);
	if (!frame.value) {
		return;
	}
	fill_rgb(frame.value);
	Ref<Image> image;
	REQUIRE(converter.convert(frame.value, image) == OK);
	REQUIRE(image.is_valid());
	if (image.is_null()) {
		return;
	}
	const Ref<Image> previous = image;
	const Vector<uint8_t> previous_pixels = image->get_data();
	const size_t extent = TestAllocation::allocation_size([]() {
		Vector<uint8_t> data;
		CHECK(data.resize(17 * 19 * 4) == OK);
	});
	{
		TestAllocation::Failure failure(extent);
		CHECK(converter.convert(frame.value, image) == ERR_OUT_OF_MEMORY);
		CHECK(TestAllocation::failures() > 0);
	}
	CHECK(image == previous);
	CHECK(image->get_data() == previous_pixels);
	CHECK(converter.convert(frame.value, image) == OK);
	CHECK(image->get_data() == previous_pixels);
}
#endif

struct AudioContext {
	AudioServer *owned = nullptr;
	AudioContext() {
		if (!AudioServer::get_singleton()) {
			AudioDriverManager::initialize(AudioDriverManager::get_driver_count() - 1);
			owned = memnew(AudioServer);
			owned->init();
		}
	}
	~AudioContext() {
		if (owned) {
			owned->finish();
			memdelete(owned);
		}
	}
};

struct SoftwareEnvironment {
	bool had = OS::get_singleton()->has_environment("GODOT_FFMPEG_FORCE_SOFTWARE");
	String previous = OS::get_singleton()->get_environment("GODOT_FFMPEG_FORCE_SOFTWARE");
	SoftwareEnvironment() { OS::get_singleton()->set_environment("GODOT_FFMPEG_FORCE_SOFTWARE", "1"); }
	~SoftwareEnvironment() {
		if (had) {
			OS::get_singleton()->set_environment("GODOT_FFMPEG_FORCE_SOFTWARE", previous);
		} else {
			OS::get_singleton()->unset_environment("GODOT_FFMPEG_FORCE_SOFTWARE");
		}
	}
};

TEST_CASE("[SceneTree][F07][FFmpegVideo] Ordinary software playback pause and seek retain the cached texture") {
	SoftwareEnvironment environment;
	AudioContext audio;
	Ref<VideoStreamMedia> stream;
	stream.instantiate();
	stream->set_file(TestUtils::get_data_path("media/ffmpeg_motion64.ogv"));
	Ref<VideoStreamPlayback> playback = stream->instantiate_playback();
	REQUIRE(playback.is_valid());
	if (playback.is_null()) {
		return;
	}
	Ref<Texture2D> texture = playback->get_texture();
	REQUIRE(texture.is_valid());
	if (texture.is_null()) {
		return;
	}
	playback->play();
	for (int i = 0; i < 6; i++) {
		playback->update(1.0 / 60.0);
	}
	CHECK(playback->get_texture() == texture);
	CHECK(texture->get_width() == 64);
	CHECK(texture->get_height() == 64);
	const double before_pause = playback->get_playback_position();
	CHECK(before_pause > 0.0);
	playback->set_paused(true);
	playback->update(0.25);
	CHECK(playback->get_playback_position() == before_pause);
	playback->seek(0.05);
	CHECK(playback->get_playback_position() == doctest::Approx(0.05));
	playback->set_paused(false);
	playback->update(1.0 / 60.0);
	CHECK(playback->get_texture() == texture);
	playback->stop();
	CHECK_FALSE(playback->is_playing());
}
} // namespace TestFFmpegVideoAllocation
#endif
