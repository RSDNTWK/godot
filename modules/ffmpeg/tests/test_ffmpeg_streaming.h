/**************************************************************************/
/*  test_ffmpeg_streaming.h                                             */
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

#pragma once

#include "modules/modules_enabled.gen.h"
#ifdef MODULE_MEDIA_STREAM_ENABLED
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/marshalls.h"
#include "core/object/callable_mp.h"
#include "core/object/message_queue.h"
#include "core/os/os.h"
#include "scene/gui/video_stream_player.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "servers/audio/audio_driver_dummy.h"
#include "servers/audio/audio_sample_clock.h"
#include "servers/audio/audio_server.h"
#include "tests/signal_watcher.h"
#include "tests/test_macros.h"

#include "modules/ffmpeg/audio_stream_pcm_decoder.h"
#include "modules/ffmpeg/ffmpeg_adaptive_quality.h"
#include "modules/ffmpeg/ffmpeg_async_playback.h"
#include "modules/ffmpeg/ffmpeg_file_io.h"
#include "modules/ffmpeg/ffmpeg_frame_queue.h"
#include "modules/ffmpeg/ffmpeg_frame_scaler.h"
#include "modules/ffmpeg/ffmpeg_media_metadata.h"
#include "modules/ffmpeg/ffmpeg_packet_reader.h"
#include "modules/ffmpeg/ffmpeg_quality_lookup.h"
#include "modules/ffmpeg/media_source_policy.h"
#include "modules/ffmpeg/media_stream_player.h"
#include "modules/ffmpeg/video_stream_media.h"
#include "modules/media_stream/media_http.h"
#include "modules/media_stream/tests/media_stream_test_utils.h"
#include "modules/media_stream/tests/test_media_http.h"

#include <cstring>

struct FFmpegRemoteIOTestAccess {
	static int open_secondary(FFmpegRemoteIO &p_parent, const String &p_url) {
		AVFormatContext context = {};
		context.opaque = &p_parent;
		AVIOContext *child = nullptr;
		const int result = FFmpegRemoteIO::open_secondary(&context, &child, p_url.utf8().get_data(), AVIO_FLAG_READ, nullptr);
		if (child) {
			FFmpegRemoteIO::close_secondary(&context, child);
		}
		return result;
	}
};

Ref<AudioStreamPlayback> ffmpeg_test_audio_packet_reader(const Vector<uint8_t> &p_wave, FFmpegPacketReader::TestReadCallback p_read, void *p_userdata, const SafeFlag *p_seek_cancel = nullptr, double p_seek_from = 0.0);
Dictionary ffmpeg_test_startup_frames(const Ref<VideoStreamPlayback> &p_playback, double p_fps, const SafeFlag &p_cancel,
		VideoStreamPlayback::AudioPlaybackPositionCallback p_restore_clock = nullptr, void *p_restore_userdata = nullptr);
bool ffmpeg_test_video_horizon_waiting(bool p_finite_video, double p_audio_position, double p_presentable_position, uint64_t p_without_progress_usec);
bool ffmpeg_test_packet_source_starved(bool p_waiting, int p_queued_pcm, int64_t p_accepted_frames, double p_origin, double p_audible_position, int p_mix_rate, bool p_has_clock = true, bool p_has_audio = true);

namespace TestNativeStreaming {

TEST_CASE("[NativeStreaming][HTTPCompatibility] Secondary FFmpeg requests retain their actual parent scheme policy") {
	for (bool initial_http : { false, true }) {
		for (bool upgrade : { false, true }) {
			TestMediaHTTP::ScriptedHTTP fixture;
			TestMediaHTTP::ScriptedReply reply;
			if (initial_http && upgrade) {
				reply.code = 302;
				reply.location = "https://8.8.8.8/master.m3u8";
				fixture.replies.push_back(reply);
				reply.code = 200;
				reply.location = String();
			}
			fixture.replies.push_back(reply);
			fixture.replies.push_back(reply);
			TestMediaHTTP::ScriptedFactory factory(fixture);
			SafeFlag cancel;
			FFmpegRemoteIO parent(&cancel, Dictionary());
			parent.set_allow_http(true);
			REQUIRE(parent.open(String(initial_http ? "http://" : "https://") + "8.8.8.8/master.m3u8") == 0);
			const int before = fixture.connections;
			const int result = FFmpegRemoteIOTestAccess::open_secondary(parent, "http://8.8.8.8/segment.ts");
			if (initial_http && !upgrade) {
				CHECK(result == 0);
				CHECK(fixture.connections == before + 1);
				CHECK(parent.is_http_transport());
			} else {
				CHECK(result < 0);
				CHECK(fixture.connections == before);
				CHECK_FALSE(parent.is_http_transport());
			}
		}
	}
}

TEST_CASE("[NativeStreaming] PCM and native media accept valid HTTPS custom ports without admitting invalid URL forms") {
	for (const String &url : { String("https://example.com/video"), String("https://example.com:443/video"), String("https://example.com:8443/video"), String("https://example.com:65535/video"), String("https://[2606:4700:4700::1111]:9443/video") }) {
		CHECK(MediaSourcePolicy::https_url(url));
	}
	for (const String &url : { String("http://example.com:8443/video"), String("https://example.com:0/video"), String("https://example.com:65536/video"), String("https://example.com:4294975739/video"), String("https://example.com:-4294967295/video"), String("https://user@example.com:8443/video"), String("https://example.com:8443/video#fragment"), String("https://example.com:8443/video\r\nHeader: value") }) {
		CHECK_FALSE(MediaSourcePolicy::https_url(url));
	}
}

struct QualityLookupFixture {
	SafeFlag entered;
	SafeFlag allowed;
	SafeFlag cancelled;
	SafeFlag finished;
	bool fail = false;
	static bool resolve(void *p_userdata, const String &p_source, const SafeFlag &p_cancel, int p_limit, FFmpegQualityLookup::Result &r_result) {
		QualityLookupFixture *self = static_cast<QualityLookupFixture *>(p_userdata);
		self->entered.set();
		while (!self->allowed.is_set() && !p_cancel.is_set()) {
			OS::get_singleton()->delay_usec(1000);
		}
		self->cancelled.set_to(p_cancel.is_set());
		r_result.video = p_source + "/" + itos(p_limit);
		r_result.metadata["video_height"] = p_limit;
		r_result.message = self->fail ? "lookup failed" : "";
		self->finished.set();
		return !p_cancel.is_set() && !self->fail;
	}
};

static bool wait_quality_flag(const SafeFlag &p_flag) {
	const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
	while (!p_flag.is_set() && OS::get_singleton()->get_ticks_usec() < deadline) {
		OS::get_singleton()->delay_usec(1000);
	}
	return p_flag.is_set();
}

TEST_CASE("[NativeStreaming][AdaptiveQuality] Lower rendition lookup does not block its owner and cancellation retires results") {
	QualityLookupFixture fixture;
	FFmpegQualityLookup lookup;
	FFmpegQualityLookup::Result result;
	CHECK(lookup.start("", 7, 720, QualityLookupFixture::resolve, &fixture) == ERR_INVALID_PARAMETER);
	REQUIRE(lookup.start("https://example.com/source", 7, 720, QualityLookupFixture::resolve, &fixture) == OK);
	REQUIRE(wait_quality_flag(fixture.entered));
	CHECK(lookup.is_running());
	CHECK_FALSE(lookup.is_ready());
	CHECK_FALSE(lookup.take(result));
	CHECK(lookup.start("https://example.com/replacement", 8, 480, QualityLookupFixture::resolve, &fixture) == ERR_BUSY);
	// Reaching these checks while the callback remains blocked verifies that
	// start/take leave the owner available for decode and presentation work.
	fixture.allowed.set();
	REQUIRE(wait_quality_flag(fixture.finished));
	const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
	bool taken = false;
	while (!(taken = lookup.take(result)) && OS::get_singleton()->get_ticks_usec() < deadline) {
		OS::get_singleton()->delay_usec(1000);
	}
	REQUIRE(taken);
	CHECK(result.found);
	CHECK(result.generation == 7);
	CHECK(result.limit == 720);
	CHECK(result.video == "https://example.com/source/720");
	CHECK_FALSE(lookup.is_running());
	CHECK_FALSE(lookup.is_ready());
	CHECK_FALSE(lookup.take(result));

	fixture.entered.clear();
	fixture.allowed.clear();
	fixture.finished.clear();
	REQUIRE(lookup.start("https://example.com/retired", 8, 480, QualityLookupFixture::resolve, &fixture) == OK);
	REQUIRE(wait_quality_flag(fixture.entered));
	lookup.cancel_and_wait();
	CHECK(fixture.cancelled.is_set());
	CHECK_FALSE(lookup.is_running());
	CHECK_FALSE(lookup.is_ready());
	CHECK_FALSE(lookup.take(result));
	CHECK(result.generation == 7); // No late result from the retired generation.

	fixture.entered.clear();
	fixture.finished.clear();
	fixture.allowed.set();
	fixture.fail = true;
	REQUIRE(lookup.start("https://example.com/failed", 9, 360, QualityLookupFixture::resolve, &fixture) == OK);
	REQUIRE(wait_quality_flag(fixture.finished));
	lookup.cancel_and_wait(); // A completed, superseded result is retired too.
	CHECK_FALSE(lookup.take(result));
	CHECK(result.generation == 7);
	fixture.finished.clear();
	REQUIRE(lookup.start("https://example.com/failed", 11, 360, QualityLookupFixture::resolve, &fixture) == OK);
	REQUIRE(wait_quality_flag(fixture.finished));
	const uint64_t failure_deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
	taken = false;
	while (!(taken = lookup.take(result)) && OS::get_singleton()->get_ticks_usec() < failure_deadline) {
		OS::get_singleton()->delay_usec(1000);
	}
	REQUIRE(taken);
	CHECK_FALSE(result.found);
	CHECK(result.generation == 11);
	CHECK(result.message == "lookup failed");

	fixture.entered.clear();
	fixture.allowed.clear();
	fixture.cancelled.clear();
	{
		FFmpegQualityLookup scoped;
		REQUIRE(scoped.start("https://example.com/closing", 10, 240, QualityLookupFixture::resolve, &fixture) == OK);
		REQUIRE(wait_quality_flag(fixture.entered));
	} // Destruction cancels and joins before the job's storage goes away.
	CHECK(fixture.cancelled.is_set());
}

TEST_CASE("[NativeStreaming][FrameScaler] Bounded threaded conversion preserves bilinear pixels across strides, slices and frame replacement") {
	FFmpegFrameScaler converter;
	const int sizes[][2] = { { 96, 64 }, { 70, 50 }, { 127, 95 }, { 192, 98 } };
	for (const auto &size : sizes) {
		AVFrame *source = av_frame_alloc();
		AVFrame *reference = av_frame_alloc();
		AVFrame *converted = av_frame_alloc();
		REQUIRE(source);
		REQUIRE(reference);
		REQUIRE(converted);
		source->width = reference->width = converted->width = size[0];
		source->height = reference->height = converted->height = size[1];
		source->format = AV_PIX_FMT_NV12;
		reference->format = converted->format = AV_PIX_FMT_RGBA;
		REQUIRE(av_frame_get_buffer(source, 256) == 0);
		REQUIRE(av_frame_get_buffer(reference, 32) == 0);
		REQUIRE(av_frame_get_buffer(converted, 32) == 0);
		SwsContext *scalar = sws_getContext(size[0], size[1], AV_PIX_FMT_NV12, size[0], size[1], AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
		REQUIRE(scalar);
		for (int pass = 0; pass < 2; pass++) {
			for (int y = 0; y < size[1]; y++) {
				for (int x = 0; x < size[0]; x++) {
					source->data[0][y * source->linesize[0] + x] = 16 + (x + y + pass * 37) % 220;
				}
			}
			for (int y = 0; y < (size[1] + 1) / 2; y++) {
				for (int x = 0; x < (size[0] + 1) / 2; x++) {
					source->data[1][y * source->linesize[1] + x * 2] = 16 + (x * 3 + y + pass * 17) % 225;
					source->data[1][y * source->linesize[1] + x * 2 + 1] = 16 + (x + y * 3 + pass * 11) % 225;
				}
			}
			REQUIRE(sws_scale(scalar, source->data, source->linesize, 0, size[1], reference->data, reference->linesize) == size[1]);
			REQUIRE(converter.convert(source, converted) == 0);
			CHECK(converter.get_threads() > 0);
			CHECK(converter.get_threads() <= 2);
			int maximum_difference = 0;
			for (int y = 0; y < size[1]; y++) {
				for (int x = 0; x < size[0] * 4; x++) {
					const int difference = int(reference->data[0][y * reference->linesize[0] + x]) - int(converted->data[0][y * converted->linesize[0] + x]);
					maximum_difference = MAX(maximum_difference, difference < 0 ? -difference : difference);
				}
			}
			INFO("Bilinear pixel comparison: ", size[0], "x", size[1], ", pass ", pass, ", maximum difference ", maximum_difference);
			CHECK(maximum_difference <= 1);
		}
		CHECK(converter.convert(nullptr, converted) == AVERROR(EINVAL));
		converted->width++;
		CHECK(converter.convert(source, converted) == AVERROR(EINVAL));
		converted->width--;
		const int saved_width = source->width;
		const int saved_height = source->height;
		source->width = converted->width = 40000;
		source->height = converted->height = 40000;
		CHECK(converter.convert(source, converted) == AVERROR(EINVAL));
		source->width = converted->width = saved_width;
		source->height = converted->height = saved_height;
		converter.reset();
		CHECK(converter.get_threads() == 0);
		REQUIRE(converter.convert(source, converted) == 0);
		sws_freeContext(scalar);
		av_frame_free(&source);
		av_frame_free(&reference);
		av_frame_free(&converted);
	}
}

TEST_CASE("[NativeStreaming][FrameQueue] Presentation waits for timestamps, keeps order, bounds memory and clears on generation change") {
	FFmpegFrameQueue frames;
	Ref<Image> image = Image::create_empty(16, 16, false, Image::FORMAT_RGBA8);
	REQUIRE(image.is_valid());
	CHECK(frames.push(image, 0.01) == OK);
	CHECK(frames.push(image, 0.02) == OK);
	CHECK(frames.push(image, 0.03) == OK);
	CHECK(frames.push(image, 0.04) == OK);
	CHECK_FALSE(frames.has_room());
	CHECK(frames.push(image, 0.05) == ERR_BUSY);
	double position = -1.0;
	int discarded = -1;
	CHECK(frames.take_due(0.005, position, discarded).is_null());
	CHECK(frames.size() == 4);
	CHECK(frames.take_due(0.02, position, discarded).is_valid());
	CHECK(position == 0.02);
	CHECK(discarded == 1);
	CHECK(frames.size() == 2);
	frames.clear();
	CHECK(frames.size() == 0);
	CHECK(frames.get_bytes() == 0);
	CHECK(frames.take_due(100.0, position, discarded).is_null());
	// Two 16 MiB frames exhaust the byte budget before the four-frame limit.
	Ref<Image> large = Image::create_empty(2048, 2048, false, Image::FORMAT_RGBA8);
	REQUIRE(large.is_valid());
	CHECK(frames.push(large, 1.0) == OK);
	CHECK(frames.push(large, 2.0) == OK);
	CHECK_FALSE(frames.has_room());
	CHECK(frames.push(large, 3.0) == ERR_BUSY);
	CHECK(frames.get_bytes() == 32 * 1024 * 1024);
	frames.clear();
	// A single supported larger frame is allowed, without also retaining others.
	Ref<Image> larger = Image::create_empty(2048, 4097, false, Image::FORMAT_RGBA8);
	REQUIRE(larger.is_valid());
	CHECK(frames.push(larger, 4.0) == OK);
	CHECK_FALSE(frames.has_room());
	CHECK(frames.push(image, 5.0) == ERR_BUSY);
	frames.clear();
	CHECK(frames.push(Ref<Image>(), 0.0) == ERR_INVALID_DATA);
}

TEST_CASE("[NativeStreaming][AudioSampleClock] Interpolation is monotonic, bounded by real samples, and resets for seeks") {
	AudioSampleClock clock;
	clock.reset(0.0);
	CHECK(clock.update(0.0, 10.0, 0.01) == 0.0);
	const double block = 512.0 / 48000.0;
	CHECK(Math::is_equal_approx(clock.update(block, 0.0, 0.01), block - 0.01));
	CHECK(Math::is_equal_approx(clock.update(block, 0.005, 0.01), block - 0.005));
	CHECK(Math::is_equal_approx(clock.update(block, 0.020, 0.01), block));
	// A silent mix callback resets its elapsed time but provides no samples.
	// Neither clock regression nor extrapolation through the underrun is valid.
	CHECK(Math::is_equal_approx(clock.update(block, 0.0, 0.01), block));
	CHECK(Math::is_equal_approx(clock.update(block, 5.0, 0.01), block));
	CHECK(clock.update(2.0 * block, 0.0, 0.01) > block);
	clock.reset(30.0);
	CHECK(clock.update(30.0, 100.0, 0.01) == 30.0);
	CHECK(Math::is_equal_approx(clock.update(30.0 + block, 0.005, 0.01), 30.0 + block - 0.005));
	clock.reset(2.0);
	// Speed scaling is applied to all source-time inputs by the player.
	CHECK(Math::is_equal_approx(clock.update(2.0 + block * 2.0, 0.005 * 2.0, 0.01 * 2.0), 2.0 + block * 2.0 - 0.01));
}

TEST_CASE("[NativeStreaming][FrameQueue] Render callback jitter preserves sequential frames and real lateness catches up") {
	FFmpegFrameQueue frames;
	Ref<Image> image = Image::create_empty(16, 16, false, Image::FORMAT_RGBA8);
	const double interval = 1.0 / 60.0;
	int produced = 0;
	for (int tick = 0; tick < 120; tick++) {
		while (produced < 120 && frames.has_room()) {
			REQUIRE(frames.push(image, produced++ * interval) == OK);
		}
		const double clock = tick * interval + (tick % 2 ? -0.002 : 0.002);
		double position = -1.0;
		int discarded = -1;
		REQUIRE(frames.take_due(clock, position, discarded, interval).is_valid());
		CHECK(Math::is_equal_approx(position, tick * interval));
		CHECK(discarded == 0);
	}
	CHECK(frames.size() == 0);
	for (int index = 0; index < 4; index++) {
		REQUIRE(frames.push(image, index * interval) == OK);
	}
	double position = -1.0;
	int discarded = -1;
	REQUIRE(frames.take_due(0.06, position, discarded, interval).is_valid());
	CHECK(Math::is_equal_approx(position, 0.05));
	CHECK(discarded == 3);
	CHECK(frames.size() == 0);
	REQUIRE(frames.push(image, 1.0) == OK);
	CHECK(frames.take_due(0.98, position, discarded, interval).is_null());
	CHECK(frames.size() == 1);
	CHECK(discarded == 0);
}

TEST_CASE("[NativeStreaming][AudioSampleClock] Different server and device block sizes preserve a continuous audible clock") {
	AudioSampleClock clock;
	clock.reset(0.0);
	for (uint64_t tick = 1; tick <= 100; ++tick) {
		const uint64_t delivered = tick * 480;
		const uint64_t mixed = ((delivered + 511) / 512) * 512;
		const double source_end = double(mixed) / 48000.0;
		const double expected = double(delivered - 480) / 48000.0;
		CHECK(Math::is_equal_approx(clock.update_scheduled(source_end, mixed, delivered, 0.0, 0.01, 48000.0, 1.0), expected));
		CHECK(Math::is_equal_approx(clock.update_scheduled(source_end, mixed, delivered, 0.005, 0.01, 48000.0, 1.0), expected + 0.005));
	}
	// Partial final audio and subsequent silent driver buffers must reach the
	// real source end instead of freezing one output-latency interval before it.
	clock.reset(0.0);
	CHECK(clock.update_scheduled(100.0 / 48000.0, 100, 480, 0.0, 0.01, 48000.0, 1.0) == 0.0);
	CHECK(Math::is_equal_approx(clock.update_scheduled(100.0 / 48000.0, 100, 960, 0.0, 0.01, 48000.0, 1.0), 100.0 / 48000.0));
	// New source data after a silent gap cannot rewind the playback position.
	CHECK(Math::is_equal_approx(clock.update_scheduled(612.0 / 48000.0, 5632, 5280, 0.0, 0.01, 48000.0, 1.0), 100.0 / 48000.0));
	CHECK(clock.update_scheduled(612.0 / 48000.0, 5632, 5760, 0.0, 0.01, 48000.0, 1.0) > 100.0 / 48000.0);
}

TEST_CASE("[NativeStreaming][AdaptiveQuality] Sustained pressure lowers one rung and brief stalls, pause and cooldown do not oscillate") {
	CHECK(FFmpegAdaptiveQuality::lower_height(1080) == 720);
	CHECK(FFmpegAdaptiveQuality::lower_height(720) == 480);
	CHECK(FFmpegAdaptiveQuality::lower_height(480) == 360);
	CHECK(FFmpegAdaptiveQuality::lower_height(360) == 240);
	CHECK(FFmpegAdaptiveQuality::lower_height(240) == 144);
	CHECK(FFmpegAdaptiveQuality::lower_height(144) == 0);
	CHECK(FFmpegAdaptiveQuality::lower_height(0) == 0);
	FFmpegAdaptiveQuality quality;
	uint64_t now = 1000000;
	quality.opened(now);
	int switches = 0;
	for (int i = 0; i < 50; ++i) {
		now += 100000;
		CHECK(quality.observe(now, 1080, true, false) == 0);
	}
	// One second of pressure is erased by a subsequent healthy second.
	for (int i = 0; i < 20; ++i) {
		now += 100000;
		CHECK(quality.observe(now, 1080, i < 10, false) == 0);
	}
	for (int i = 0; i < 31; ++i) {
		now += 100000;
		const int lower = quality.observe(now, 1080, true, false);
		if (lower) {
			CHECK(lower == 720);
			switches++;
		}
	}
	CHECK(switches == 1);
	quality.opened(now);
	for (int i = 0; i < 290; ++i) {
		now += 100000;
		CHECK(quality.observe(now, 720, true, false) == 0);
	}
	now += 1000000;
	CHECK(quality.observe(now, 720, true, false) == 480);
	// Suspension discards pressure and gives the resumed input a new warmup.
	now += 1000000;
	CHECK(quality.observe(now, 480, true, true) == 0);
	now += 1000000;
	CHECK(quality.observe(now, 480, true, false) == 0);
	quality.disable();
	now += 60000000;
	CHECK(quality.observe(now, 480, true, false) == 0);
}

TEST_CASE("[NativeStreaming][AdaptiveQuality] Dropped-frame cadence triggers downshift but an application FPS cap does not") {
	for (bool application_cap : { false, true }) {
		FFmpegAdaptiveQuality quality;
		uint64_t now = 1000000;
		quality.opened(now);
		int switches = 0;
		for (int i = 1; i <= 160; ++i) {
			now += 100000;
			const int lower = quality.observe(now, 1080, false, false, i * 3, i * 0.1, 60.0, i * (application_cap ? 3 : 6));
			if (lower) {
				CHECK(lower == 720);
				switches++;
			}
		}
		CHECK(switches == (application_cap ? 0 : 1));
	}
}

TEST_CASE("[NativeStreaming][AdaptiveQuality] Backpressure slowing both clocks triggers downshift without treating requested slow playback as a failure") {
	for (bool requested_slow_playback : { false, true }) {
		FFmpegAdaptiveQuality quality;
		uint64_t now = 1000000;
		quality.opened(now);
		int switches = 0;
		for (int i = 1; i <= 160; ++i) {
			now += 100000;
			const double source_clock = i * 0.02;
			const double requested_clock = i * (requested_slow_playback ? 0.02 : 0.1);
			const int lower = quality.observe(now, 1080, false, false, i * 3 / 5, source_clock, 30.0, i * 6, requested_clock);
			if (lower) {
				CHECK(lower == 720);
				switches++;
			}
		}
		CHECK(switches == (requested_slow_playback ? 0 : 1));
	}
}
static void initialize_audio() {
	if (!AudioServer::get_singleton()) {
		AudioDriverManager::initialize(AudioDriverManager::get_driver_count() - 1);
		AudioServer *server = memnew(AudioServer);
		server->init();
	}
}
struct AudioSink {
	int64_t frames = 0;
	double energy = 0;
	int64_t live_available_frames = -1;
	int clock_mix_rate = 0;
	double clock_origin = 0;
	static double read_consumed_clock(void *p_self) {
		const AudioSink *self = static_cast<const AudioSink *>(p_self);
		return self->clock_mix_rate > 0 ? self->clock_origin + double(self->frames) / self->clock_mix_rate : -1.0;
	}
	static int mix(void *p_self, const float *p_samples, int p_frames) {
		AudioSink *self = static_cast<AudioSink *>(p_self);
		const int accepted = self->live_available_frames < 0 ? p_frames : int(MIN(int64_t(p_frames), self->live_available_frames));
		if (self->live_available_frames >= 0) {
			self->live_available_frames -= accepted;
		}
		self->frames += accepted;
		for (int i = 0; i < accepted * 2; i++) {
			self->energy += Math::abs(p_samples[i]);
		}
		return accepted;
	}
};

static Vector<uint8_t> packet_test_wave(int p_rate, int p_frames = 2048);

struct LookaheadSink {
	Vector<float> samples;
	int consumed = 0;
	int rate = 0;
	double origin = 0.0;
	static double clock(void *p_self) {
		const LookaheadSink *self = static_cast<LookaheadSink *>(p_self);
		return self->origin + double(self->consumed) / self->rate;
	}
	static int mix(void *p_self, const float *p_samples, int p_frames) {
		LookaheadSink *self = static_cast<LookaheadSink *>(p_self);
		const int frames = self->samples.size() / 2;
		const int accepted = MIN(p_frames, MAX(0, self->rate / 4 - (frames - self->consumed)));
		self->samples.resize((frames + accepted) * 2);
		memcpy(self->samples.ptrw() + frames * 2, p_samples, accepted * 2 * sizeof(float));
		return accepted;
	}
};

TEST_CASE("[SceneTree][NativeStreaming][VideoLookahead][Local] Finite streaming queues bounded real PCM and replaces it on seek" * doctest::skip()) {
	const String path = OS::get_singleton()->get_environment("GODOT_MEDIA_LOOKAHEAD_FILE");
	REQUIRE(FileAccess::exists(path));
	initialize_audio();
	SafeFlag cancel;
	String error;
	// Independent, deterministic PCM input isolates queue ownership from codec
	// overlap/preroll differences between fresh and retained compressed seeks.
	const int rate = AudioServer::get_singleton()->get_mix_rate();
	Vector<uint8_t> wave = packet_test_wave(rate);
	wave.resize(44 + rate * 2 * 4);
	uint8_t *bytes = wave.ptrw();
	encode_uint32(wave.size() - 8, bytes + 4);
	encode_uint32(wave.size() - 44, bytes + 40);
	for (int frame = 0; frame < rate * 2; ++frame) {
		encode_uint16(uint16_t(((frame % 257) - 128) * 160), bytes + 44 + frame * 4);
		encode_uint16(uint16_t(((frame % 239) - 119) * 160), bytes + 46 + frame * 4);
	}
	Ref<FileAccess> audio_file = FileAccess::create_temp(FileAccess::READ_WRITE, "godot-media-lookahead", "wav");
	REQUIRE(audio_file.is_valid());
	audio_file->store_buffer(wave);
	audio_file->flush();
	const String audio_path = audio_file->get_path();
	Ref<VideoStreamPlayback> playback = ffmpeg_open_video_on_worker(path, Dictionary(), &cancel, audio_path, Dictionary(), &error);
	const CharString error_text = error.utf8();
	INFO(error_text.get_data());
	REQUIRE(playback.is_valid());
	CHECK(bool(playback->get(SNAME("_ffmpeg_async_frames"))));
	LookaheadSink sink;
	sink.rate = playback->get_mix_rate();
	REQUIRE(sink.rate > 0);
	playback->set_mix_callback(LookaheadSink::mix, &sink);
	playback->set_audio_playback_position_callback(LookaheadSink::clock, &sink);
	playback->play();
	playback->update(0.0);
	const int prefilled = sink.samples.size() / 2;
	CHECK(prefilled >= sink.rate / 5 - 1);
	CHECK(prefilled <= sink.rate / 5 + 1);
	for (int i = 0; i < 10; ++i) {
		playback->update(0.0);
	}
	CHECK(sink.samples.size() / 2 == prefilled); // Zero-delta retries cannot grow PCM.
	CHECK(LookaheadSink::clock(&sink) == 0.0); // Queued PCM is not consumed time.
	CHECK(playback->get_playback_position() < 0.1);
	sink.consumed = sink.rate / 10;
	playback->update(0.1);
	CHECK(sink.samples.size() / 2 - sink.consumed >= sink.rate / 5 - 1);
	CHECK(sink.samples.size() / 2 - sink.consumed <= sink.rate / 5 + 1);
	// Run the real bounded video producer. A short forward correction should
	// consume these queued packets without resetting the decoder to a keyframe.
	REQUIRE(ffmpeg_start_video_read_ahead(playback, &error));

	// Retire the external generation just as the asynchronous wrapper does.
	sink.samples.clear();
	sink.consumed = 0;
	sink.origin = 0.723;
	REQUIRE(ffmpeg_seek_video_on_worker(playback, sink.origin));
	CHECK(int64_t(playback->get(SNAME("_ffmpeg_forward_seek_count"))) == 1);
	CHECK(playback->get_playback_position() >= sink.origin);
	CHECK(playback->get_playback_position() < sink.origin + 0.1);
	CHECK(sink.samples.size() / 2 >= sink.rate / 5 - 1);
	CHECK(sink.samples.size() / 2 <= sink.rate / 5 + 1);
	Ref<AudioStreamPlayback> reference = ffmpeg_audio_playback_from_buffer(Vector<uint8_t>(), nullptr, audio_path, Ref<AudioStream>(), Dictionary(), &cancel);
	REQUIRE(reference.is_valid());
	reference->start(sink.origin);
	AudioFrame decoded[512];
	REQUIRE(reference->mix(decoded, 1.0, 512) == 512);
	REQUIRE(sink.samples.size() >= 1024);
	double energy = 0;
	double difference = 0;
	for (int i = 0; i < 512; ++i) {
		energy += Math::abs(sink.samples[i * 2]) + Math::abs(sink.samples[i * 2 + 1]);
		difference = MAX(difference, Math::abs(double(sink.samples[i * 2]) - decoded[i].left));
		difference = MAX(difference, Math::abs(double(sink.samples[i * 2 + 1]) - decoded[i].right));
	}
	CHECK(energy > 0.1);
	CHECK(difference < 0.00001); // Real PCM at the new target, not padding/old samples.
	CHECK(Math::is_equal_approx(LookaheadSink::clock(&sink), sink.origin));
	CHECK(int(playback->get(SNAME("stream_error"))) == OK);

	// EOF in the decoder is not EOF in the device queue. Keep the real audio
	// clock active until the queued tail is consumed, without padding to 200ms.
	sink.samples.clear();
	sink.consumed = 0;
	sink.origin = playback->get_length() - 0.1;
	REQUIRE(sink.origin > 0.0);
	playback->set_audio_playback_position(sink.origin);
	REQUIRE(ffmpeg_seek_video_on_worker(playback, sink.origin));
	CHECK(bool(playback->get(SNAME("_ffmpeg_audio_active"))));
	Vector<AudioFrame> tail;
	tail.resize(sink.rate);
	reference->start(sink.origin);
	const int tail_frames = reference->mix(tail.ptrw(), 1.0, sink.rate);
	CHECK(tail_frames > 0);
	CHECK(tail_frames < sink.rate / 5);
	CHECK(sink.samples.size() / 2 == tail_frames);
	sink.consumed = sink.samples.size() / 2;
	playback->set_audio_playback_position(LookaheadSink::clock(&sink));
	CHECK_FALSE(bool(playback->get(SNAME("_ffmpeg_audio_active"))));
}

#ifdef DEV_ENABLED
TEST_CASE("[SceneTree][NativeStreaming][VideoCatchup][Local] Async catch-up preserves moving-clock publication, seek images and EOF tails" * doctest::skip()) {
	const String directory = OS::get_singleton()->get_environment("GODOT_MEDIA_CATCHUP_DIR");
	initialize_audio();
	for (const int fps : { 5, 10, 24, 60 }) {
		for (const int audio_mode : { 0, 1, 2 }) { // Separate, embedded, absent.
			const CharString label = vformat("%d fps, audio mode %d", fps, audio_mode).utf8();
			INFO(label.get_data());
			const String path = directory.path_join(vformat("video-%d-%s.mkv", fps, audio_mode == 1 ? "audio" : "silent"));
			REQUIRE(FileAccess::exists(path));
			Ref<FileAccess> audio_file;
			if (audio_mode == 0) {
				const int rate = AudioServer::get_singleton()->get_mix_rate();
				Vector<uint8_t> wave = packet_test_wave(rate);
				wave.resize(44 + rate * 2 * 4);
				uint8_t *bytes = wave.ptrw();
				encode_uint32(wave.size() - 8, bytes + 4);
				encode_uint32(wave.size() - 44, bytes + 40);
				memset(bytes + 44, 0x20, wave.size() - 44);
				audio_file = FileAccess::create_temp(FileAccess::READ_WRITE, "godot-media-catchup", "wav");
				REQUIRE(audio_file.is_valid());
				audio_file->store_buffer(wave);
				audio_file->flush();
			}
			SafeFlag cancel;
			String error;
			Ref<VideoStreamPlayback> playback = ffmpeg_open_video_on_worker(path, Dictionary(), &cancel, audio_file.is_valid() ? audio_file->get_path() : String(), Dictionary(), &error);
			REQUIRE(playback.is_valid());
			REQUIRE(playback->get_length() > 1.5);
			LookaheadSink sink;
			sink.rate = playback->get_mix_rate();
			playback->set_mix_callback(LookaheadSink::mix, &sink);
			playback->set_audio_playback_position_callback(LookaheadSink::clock, &sink);
			double fallback_clock = 0.0;
			auto update_at = [&](double p_clock) {
				sink.origin = p_clock;
				const double delta = audio_mode == 2 ? MAX(0.0, p_clock - fallback_clock) : 0.0;
				playback->update(delta);
				fallback_clock += delta;
			};
			auto state = [&]() -> Dictionary { return playback->get(SNAME("_ffmpeg_debug_state")); };
			auto serial = [&]() -> int64_t { return playback->get(SNAME("_ffmpeg_presented_frames")); };
			auto image = [&]() -> Ref<Image> { return playback->get(SNAME("_ffmpeg_cpu_frame")); };
			playback->play();
			const uint64_t initial_deadline = OS::get_singleton()->get_ticks_usec() + 1000000;
			while (serial() == 0 && OS::get_singleton()->get_ticks_usec() < initial_deadline) {
				update_at(0.0);
			}
			REQUIRE(serial() > 0);
			REQUIRE(ffmpeg_start_video_read_ahead(playback, &error));
			const int64_t before = serial();
			const int64_t decoded_before = state()["decoded_frames"];
			update_at(1.2);
			if (fps >= 24) {
				CHECK(serial() == before); // Obsolete dense frames avoid conversion.
			} else {
				CHECK(serial() > before); // Sparse sources must keep publishing.
			}
			const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
			while (playback->get_playback_position() < 0.95 && OS::get_singleton()->get_ticks_usec() < deadline) {
				update_at(1.2);
			}
			CHECK(playback->get_playback_position() >= 0.95);
			CHECK(playback->get_playback_position() <= 1.35);
			if (fps >= 24) {
				CHECK((serial() - before) * 2 < int64_t(state()["decoded_frames"]) - decoded_before);
			}

			// Model continuously advancing consumed time, with 350ms output
			// lag. This is inside the public 450ms sync band, not a fixed jump.
			REQUIRE(ffmpeg_seek_video_on_worker(playback, 0.0));
			fallback_clock = 0.0;
			sink.origin = 0.0;
			REQUIRE(ffmpeg_start_video_read_ahead(playback, &error, true));
			int64_t previous_serial = serial();
			Ref<Image> previous_image = image();
			double previous_pts = playback->get_playback_position();
			double worst_lag = 0.0;
			double largest_pts_gap = 0.0;
			int publications = 0;
			bool images_changed = true;
			const uint64_t moving_deadline = OS::get_singleton()->get_ticks_usec() + 3000000;
			while (playback->is_playing() && OS::get_singleton()->get_ticks_usec() < moving_deadline) {
				const double requested_clock = double(state()["last_decoded_position"]) + 0.35;
				update_at(requested_clock);
				if (serial() != previous_serial) {
					Ref<Image> current = image();
					images_changed = images_changed && current.is_valid() && !current->is_empty() && current != previous_image && current->get_data() != previous_image->get_data();
					largest_pts_gap = MAX(largest_pts_gap, playback->get_playback_position() - previous_pts);
					previous_pts = playback->get_playback_position();
					previous_image = current;
					previous_serial = serial();
					publications++;
				}
				worst_lag = MAX(worst_lag, requested_clock - playback->get_playback_position());
			}
			CHECK_FALSE(playback->is_playing());
			CHECK(publications > 5);
			CHECK(images_changed);
			CHECK(worst_lag <= 0.45);
			CHECK(largest_pts_gap <= Math::ceil(0.1 * fps) / fps + 0.000001);
			CHECK(Math::abs(playback->get_playback_position() - (2.0 - 1.0 / fps)) < 0.000001);
			CHECK(int(playback->get(SNAME("stream_error"))) == OK);

			// Verify a new actual image, not just a changed position scalar.
			for (const double target : { 0.723, 0.763 }) {
				const int64_t before_seek = serial();
				const double old_position = playback->get_playback_position();
				Ref<Image> old_image = image();
				REQUIRE(ffmpeg_seek_video_on_worker(playback, target));
				fallback_clock = target;
				CHECK(Math::abs(playback->get_playback_position() - Math::ceil(target * fps) / fps) < 0.000001);
				CHECK(serial() > before_seek);
				Ref<Image> poster = image();
				REQUIRE(poster.is_valid());
				CHECK_FALSE(poster->is_empty());
				CHECK(poster != old_image);
				if (!Math::is_equal_approx(Math::ceil(target * fps) / fps, old_position)) {
					CHECK(poster->get_data() != old_image->get_data());
				} else {
					CHECK(poster->get_data() == old_image->get_data());
				}
			}
			const double paused_position = playback->get_playback_position();
			const int64_t paused_serial = serial();
			playback->set_paused(true);
			playback->update(1.0);
			CHECK(playback->get_playback_position() == paused_position);
			CHECK(serial() == paused_serial);
			playback->set_paused(false);

			// A sparse source can have a future decoded frame queued beyond
			// the small forward target, while its last image is still at zero.
			if (fps == 5) {
				REQUIRE(ffmpeg_seek_video_on_worker(playback, 0.0));
				fallback_clock = 0.0;
				sink.origin = 0.0;
				REQUIRE(ffmpeg_start_video_read_ahead(playback, &error, true));
				const uint64_t cursor_deadline = OS::get_singleton()->get_ticks_usec() + 1000000;
				while (double(state()["last_decoded_position"]) < 0.19 && OS::get_singleton()->get_ticks_usec() < cursor_deadline) {
					update_at(0.0);
				}
				REQUIRE(double(state()["last_decoded_position"]) >= 0.19);
				CHECK(playback->get_playback_position() == 0.0);
				const int64_t forward_count = playback->get(SNAME("_ffmpeg_forward_seek_count"));
				REQUIRE(ffmpeg_seek_video_on_worker(playback, 0.123));
				CHECK(int64_t(playback->get(SNAME("_ffmpeg_forward_seek_count"))) == forward_count);
				CHECK(Math::abs(playback->get_playback_position() - 0.2) < 0.000001);
			}
		}
	}
}
#endif

static bool wait_ready(const Ref<RefCounted> &p_playback, uint64_t p_timeout_usec = 45000000) {
	uint64_t deadline = OS::get_singleton()->get_ticks_usec() + p_timeout_usec;
	while (OS::get_singleton()->get_ticks_usec() < deadline) {
		MessageQueue::get_singleton()->flush();
		if (int(p_playback->get("stream_error")) != OK) {
			return false;
		}
		if (bool(p_playback->get("stream_ready")) && !bool(p_playback->get("buffering"))) {
			return true;
		}
		OS::get_singleton()->delay_usec(2000);
	}
	return false;
}

TEST_CASE("[SceneTree][NativeStreaming][VideoCatchup][Credit] A stalled finite mixer cannot accumulate an unbounded production burst" * doctest::skip()) {
	initialize_audio();
	const String directory = OS::get_singleton()->get_environment("GODOT_MEDIA_CATCHUP_DIR");
	const String path = directory.path_join("video-60-audio.mkv");
	REQUIRE(FileAccess::exists(path));
	SafeFlag cancel;
	String error;
	Ref<VideoStreamPlayback> playback = ffmpeg_open_video_on_worker(path, Dictionary(), &cancel, String(), Dictionary(), &error);
	REQUIRE(playback.is_valid());
	LookaheadSink sink;
	sink.rate = playback->get_mix_rate();
	playback->set_mix_callback(LookaheadSink::mix, &sink);
	playback->set_audio_playback_position_callback(LookaheadSink::clock, &sink);
	playback->play();
	REQUIRE(ffmpeg_start_video_read_ahead(playback, &error));
	playback->update(3600.0);
	CHECK(double(playback->get(SNAME("_ffmpeg_audio_backlog"))) <= 1.0);
	CHECK(double(playback->get(SNAME("_ffmpeg_audio_backlog"))) >= 0.0);
	CHECK(sink.samples.size() <= sink.rate / 4 * 2);
	CHECK_FALSE(sink.samples.is_empty());
	CHECK(int(playback->get(SNAME("stream_error"))) == OK);
}

TEST_CASE("[SceneTree][NativeStreaming][VideoCatchup][Startup] Initial frame preparation preserves the prefix and detects an unsustainable decoder" * doctest::skip()) {
	initialize_audio();
	const String path = OS::get_singleton()->get_environment("GODOT_MEDIA_CATCHUP_DIR").path_join("video-60-audio.mkv");
	REQUIRE(FileAccess::exists(path));
	for (int delay_msec : { 0, 12, 40 }) {
		const String old_height = OS::get_singleton()->get_environment("GODOT_FFMPEG_TEST_SLOW_VIDEO_ABOVE_HEIGHT");
		const String old_delay = OS::get_singleton()->get_environment("GODOT_FFMPEG_TEST_VIDEO_DELAY_MSEC");
		OS::get_singleton()->set_environment("GODOT_FFMPEG_TEST_SLOW_VIDEO_ABOVE_HEIGHT", delay_msec > 0 ? "1" : "0");
		OS::get_singleton()->set_environment("GODOT_FFMPEG_TEST_VIDEO_DELAY_MSEC", itos(delay_msec));
		SafeFlag cancel;
		String error;
		Ref<VideoStreamPlayback> playback = ffmpeg_open_video_on_worker(path, Dictionary(), &cancel, String(), Dictionary(), &error);
		OS::get_singleton()->set_environment("GODOT_FFMPEG_TEST_SLOW_VIDEO_ABOVE_HEIGHT", old_height);
		OS::get_singleton()->set_environment("GODOT_FFMPEG_TEST_VIDEO_DELAY_MSEC", old_delay);
		REQUIRE(playback.is_valid());
		LookaheadSink sink;
		sink.rate = playback->get_mix_rate();
		playback->set_mix_callback(LookaheadSink::mix, &sink);
		playback->play();
		REQUIRE(ffmpeg_start_video_read_ahead(playback, &error));
		Dictionary skipped = ffmpeg_test_startup_frames(playback, 0.0, cancel);
		CHECK(int(skipped["error"]) == OK);
		CHECK(Array(skipped["images"]).is_empty());
		Dictionary result = ffmpeg_test_startup_frames(playback, 60.0, cancel);
		REQUIRE(int(result["error"]) == OK);
		CHECK(bool(result["measured"]));
		CHECK(bool(result["too_slow"]) == (delay_msec > 0));
		CHECK(int(result["sampled_frames"]) == 36);
		Array positions = result["positions"];
		Array images = result["images"];
		REQUIRE(positions.size() == 4);
		REQUIRE(images.size() == 4);
		for (int index = 0; index < 4; index++) {
			CHECK(Math::abs(double(positions[index]) - index / 60.0) < 0.000001);
			Ref<Image> image = images[index];
			REQUIRE(image.is_valid());
			CHECK_FALSE(image->is_empty());
			if (index > 0) {
				Ref<Image> previous = images[index - 1];
				CHECK(previous->get_data() != image->get_data());
			}
		}
		CHECK(sink.consumed == 0); // Preparation never pretends audio was heard.
		CHECK_FALSE(sink.samples.is_empty());
		cancel.set();
		Dictionary cancelled = ffmpeg_test_startup_frames(playback, 60.0, cancel);
		CHECK(int(cancelled["error"]) == ERR_SKIP);
		CHECK(Array(cancelled["images"]).is_empty());
	}
}

TEST_CASE("[SceneTree][NativeStreaming][VideoCatchup][Startup] In-flight preparation cancellation restores the real clock callback" * doctest::skip()) {
	initialize_audio();
	const String path = OS::get_singleton()->get_environment("GODOT_MEDIA_CATCHUP_DIR").path_join("video-60-audio.mkv");
	REQUIRE(FileAccess::exists(path));
	const String old_height = OS::get_singleton()->get_environment("GODOT_FFMPEG_TEST_SLOW_VIDEO_ABOVE_HEIGHT");
	const String old_delay = OS::get_singleton()->get_environment("GODOT_FFMPEG_TEST_VIDEO_DELAY_MSEC");
	OS::get_singleton()->set_environment("GODOT_FFMPEG_TEST_SLOW_VIDEO_ABOVE_HEIGHT", "1");
	OS::get_singleton()->set_environment("GODOT_FFMPEG_TEST_VIDEO_DELAY_MSEC", "40");
	SafeFlag transport_cancel;
	SafeFlag preparation_cancel;
	String error;
	Ref<VideoStreamPlayback> playback = ffmpeg_open_video_on_worker(path, Dictionary(), &transport_cancel, String(), Dictionary(), &error);
	OS::get_singleton()->set_environment("GODOT_FFMPEG_TEST_SLOW_VIDEO_ABOVE_HEIGHT", old_height);
	OS::get_singleton()->set_environment("GODOT_FFMPEG_TEST_VIDEO_DELAY_MSEC", old_delay);
	REQUIRE(playback.is_valid());
	LookaheadSink sink;
	sink.rate = playback->get_mix_rate();
	sink.origin = 0.123;
	playback->set_mix_callback(LookaheadSink::mix, &sink);
	playback->set_audio_playback_position_callback(LookaheadSink::clock, &sink);
	playback->play();
	REQUIRE(ffmpeg_start_video_read_ahead(playback, &error));
	Thread cancellation;
	cancellation.start([](void *p_flag) {
		OS::get_singleton()->delay_usec(10000);
		static_cast<SafeFlag *>(p_flag)->set();
	}, &preparation_cancel);
	REQUIRE(cancellation.is_started());
	const uint64_t began = OS::get_singleton()->get_ticks_usec();
	Dictionary result = ffmpeg_test_startup_frames(playback, 60.0, preparation_cancel, LookaheadSink::clock, &sink);
	cancellation.wait_to_finish();
	CHECK(int(result["error"]) == ERR_SKIP);
	CHECK(OS::get_singleton()->get_ticks_usec() - began < 1000000);
	Dictionary state = playback->get(SNAME("_ffmpeg_debug_state"));
	CHECK(Math::abs(double(state["target_clock"]) - 0.123) < 0.000001);
	CHECK(sink.consumed == 0);
}

TEST_CASE("[SceneTree][NativeStreaming][FiniteBackpressure] Brief horizon waits do not report a persistent video stall") {
	CHECK_FALSE(ffmpeg_test_video_horizon_waiting(true, 0.3, 0.2, 0));
	CHECK_FALSE(ffmpeg_test_video_horizon_waiting(true, 0.3, 0.2, 99999));
	CHECK(ffmpeg_test_video_horizon_waiting(true, 0.3, 0.2, 100000));
	CHECK(ffmpeg_test_video_horizon_waiting(true, 0.3, 0.2, 500000));
	CHECK_FALSE(ffmpeg_test_video_horizon_waiting(true, 0.24, 0.2, 500000));
	// Newly decoded progress restarts the no-progress interval even when
	// audio has already consumed the newly extended horizon.
	CHECK_FALSE(ffmpeg_test_video_horizon_waiting(true, 0.32, 0.22, 1000));
	CHECK_FALSE(ffmpeg_test_video_horizon_waiting(false, 0.15, 0.2, 99999));
	CHECK(ffmpeg_test_video_horizon_waiting(false, 0.15, 0.2, 100000));
	CHECK_FALSE(ffmpeg_test_video_horizon_waiting(false, 0.09, 0.2, 500000));
}

TEST_CASE("[SceneTree][NativeStreaming][PacketStarvation] Frontend source-frame reserve uses the audible origin and conservatively handles missing clocks") {
	constexpr int rate = 48000;
	CHECK_FALSE(ffmpeg_test_packet_source_starved(false, 0, 0, 0.0, 0.0, rate));
	CHECK_FALSE(ffmpeg_test_packet_source_starved(true, 16, 0, 0.0, 0.0, rate));
	CHECK_FALSE(ffmpeg_test_packet_source_starved(true, 0, rate, 32.123, 32.623, rate));
	CHECK_FALSE(ffmpeg_test_packet_source_starved(true, 0, rate, 32.123, 33.123 - 2.0 / rate, rate));
	CHECK(ffmpeg_test_packet_source_starved(true, 0, rate, 32.123, 33.123 - 0.5 / rate, rate));
	CHECK(ffmpeg_test_packet_source_starved(true, 0, rate, 32.123, 33.123, rate));
	// DVR-relative origins and heard source time remain in the same units at
	// non-unit playback speeds; device latency must not be subtracted again.
	CHECK_FALSE(ffmpeg_test_packet_source_starved(true, 0, rate, 500.0, 500.5, rate));
	CHECK(ffmpeg_test_packet_source_starved(true, 0, 0, 500.0, 500.0, rate));
	CHECK(ffmpeg_test_packet_source_starved(true, 0, rate, 0.0, 0.0, rate, false));
	CHECK(ffmpeg_test_packet_source_starved(true, 0, rate, 0.0, 0.0, rate, true, false));
	CHECK(ffmpeg_test_packet_source_starved(true, 0, rate, 0.0, 0.0, 0));
	CHECK(ffmpeg_test_packet_source_starved(true, 0, rate, Math::NaN, 0.0, rate));
	CHECK(ffmpeg_test_packet_source_starved(true, 0, rate, 0.0, Math::NaN, rate));
	CHECK(ffmpeg_test_packet_source_starved(true, 0, rate, 1.0, 0.0, rate));
}

struct FiniteVideoFixture {
	SafeNumeric<int> owners{ 0 };
	SafeNumeric<int> audio_limit{ 0 };
	SafeFlag produced;
	SafeFlag filled;
	SafeFlag release_video;
	SafeFlag packet_waiting;
	SafeFlag packet_wait_observed;
	SafeFlag advanced_frame;
	SafeFlag publication_blocked;
	SafeFlag release_publication;
	SafeFlag repeat_publication;
	SafeNumeric<int> repeated_serial{ 2 };
	double presentable_position = 0.2;
	float sample_value = 0.25f;
	bool hold_video = false;
	bool live = true;
	Error terminal_error = OK;
	int mix_rate = 0;
	bool empty = false;
	bool cpu_frames = false;
	bool prepared_cpu_frame = false;
	bool block_after_publication = false;
	Ref<Image> image;
};

// A finite source deliberately leaves more decoded PCM than its last video
// frame can present. This is also the terminal state when recovery exhausts
// the input before finding another usable video frame.
class FiniteVideoPlayback : public VideoStreamPlayback {
	GDCLASS(FiniteVideoPlayback, VideoStreamPlayback);
	FiniteVideoFixture *fixture = nullptr;
	bool playing = false;
	bool paused = false;
	int emitted = 0;
	bool ended = false;

protected:
	static void _bind_methods() {}

public:
	void initialize(FiniteVideoFixture *p_fixture) {
		fixture = p_fixture;
		fixture->owners.increment();
	}
	bool _get(const StringName &p_name, Variant &r_value) const {
		if (p_name == SNAME("_ffmpeg_async_frames")) {
			r_value = fixture->cpu_frames;
		} else if (p_name == SNAME("_ffmpeg_presented_frames")) {
			r_value = int64_t(fixture->repeat_publication.is_set() ? fixture->repeated_serial.get() : (fixture->advanced_frame.is_set() ? 2 : (fixture->prepared_cpu_frame || emitted > 0 ? 1 : 0)));
		} else if (p_name == SNAME("_ffmpeg_cpu_frame")) {
			r_value = fixture->prepared_cpu_frame || emitted > 0 ? fixture->image : Ref<Image>();
		} else if (p_name == SNAME("_ffmpeg_audio_active")) {
			r_value = playing;
		} else if (p_name == SNAME("_ffmpeg_live_hls")) {
			r_value = fixture->live;
		} else if (p_name == SNAME("_ffmpeg_packet_waiting")) {
			r_value = fixture->packet_waiting.is_set();
			if (fixture->packet_waiting.is_set()) {
				fixture->packet_wait_observed.set();
			}
		} else if (p_name == SNAME("_ffmpeg_presentable_position")) {
			r_value = fixture->presentable_position;
		} else if (p_name == SNAME("stream_error")) {
			r_value = int(ended ? fixture->terminal_error : OK);
		} else if (p_name == SNAME("_ffmpeg_error_message")) {
			r_value = String();
		} else if (p_name == SNAME("_ffmpeg_metadata")) {
			r_value = Dictionary();
		} else {
			return false;
		}
		return true;
	}
	void play() override { playing = true; }
	void stop() override { playing = false; }
	bool is_playing() const override { return playing; }
	void set_paused(bool p_paused) override { paused = p_paused; }
	bool is_paused() const override { return paused; }
	double get_length() const override { return fixture->live ? 0.0 : 1.0; }
	double get_playback_position() const override { return fixture->advanced_frame.is_set() ? 0.3 : (fixture->prepared_cpu_frame || emitted > 0 ? 0.2 : 0.0); }
	Ref<Texture2D> get_texture() const override { return Ref<Texture2D>(); }
	int get_channels() const override { return 2; }
	int get_mix_rate() const override { return fixture->mix_rate; }
	void update(double) override {
		if (!playing || paused || !mix_callback) {
			return;
		}
		float samples[2048];
		for (float &sample : samples) {
			sample = fixture->sample_value;
		}
		const int limit = fixture->audio_limit.get() > 0 ? fixture->audio_limit.get() : fixture->mix_rate;
		while (!fixture->empty && emitted < limit) {
			const int count = MIN(1024, limit - emitted);
			const int accepted = mix_callback(mix_udata, samples, count);
			emitted += accepted;
			if (accepted < count) {
				return;
			}
		}
		fixture->filled.set();
		if (fixture->block_after_publication) {
			if (!fixture->advanced_frame.is_set()) {
				OS::get_singleton()->delay_usec(120000);
				fixture->advanced_frame.set();
				return;
			}
			fixture->publication_blocked.set();
			while (!fixture->release_publication.is_set()) {
				OS::get_singleton()->delay_usec(1000);
			}
			if (fixture->repeat_publication.is_set()) {
				fixture->repeated_serial.increment();
			}
			return;
		}
		if (fixture->hold_video && !fixture->release_video.is_set()) {
			return;
		}
		playing = false;
		ended = true;
		fixture->produced.set();
	}
	~FiniteVideoPlayback() override {
		if (fixture) {
			fixture->owners.decrement();
		}
	}
	static Ref<VideoStreamPlayback> create(void *p_userdata) {
		Ref<FiniteVideoPlayback> result;
		result.instantiate();
		result->initialize(static_cast<FiniteVideoFixture *>(p_userdata));
		return result;
	}
};

TEST_CASE("[SceneTree][NativeStreaming][FramePublication] A queued image extends the audio horizon before its decode batch finishes") {
	for (double predicted_horizon : { 0.2, 0.3 }) {
		CAPTURE(predicted_horizon);
		initialize_audio();
		FiniteVideoFixture fixture;
		fixture.live = false;
		fixture.cpu_frames = true;
		fixture.prepared_cpu_frame = true;
		fixture.block_after_publication = true;
		fixture.presentable_position = predicted_horizon;
		fixture.mix_rate = AudioServer::get_singleton()->get_mix_rate();
		fixture.image = Image::create_empty(2, 2, false, Image::FORMAT_RGBA8);
		Ref<VideoStreamPlayback> playback = ffmpeg_test_async_video(FiniteVideoPlayback::create, &fixture);
		struct ReleasePublication {
			SafeFlag &flag;
			~ReleasePublication() { flag.set(); }
		} release_on_exit{ fixture.release_publication };
		REQUIRE(playback.is_valid());
		AudioSink sink;
		playback->set_mix_callback(AudioSink::mix, &sink);
		playback->set_audio_playback_position(0.0);
		playback->set_audio_output_latency(0.0);
		playback->play();
		REQUIRE(wait_ready(playback, 2000000));
		REQUIRE(wait_quality_flag(fixture.filled));
		playback->update(0.0); // Each call delivers at most sixteen 1024-frame chunks.
		playback->update(0.0); // Fill the remaining part of the initial allowance.
		REQUIRE(sink.frames == int64_t((predicted_horizon + 0.1) * fixture.mix_rate));
		REQUIRE(wait_quality_flag(fixture.publication_blocked));
		// The worker queued the 0.3s image, then blocked inside its next update.
		// Its end-of-batch snapshot may already predict that PTS, but it cannot
		// replace the timestamp of actual CPU-image publication.
		playback->set_audio_playback_position(0.35);
		playback->update(0.0);
		MessageQueue::get_singleton()->flush();
		CHECK(Math::abs(playback->get_playback_position() - 0.3) < 0.000001);
		CHECK(sink.frames == int64_t(0.4 * fixture.mix_rate));
		CHECK_FALSE(bool(playback->get("buffering")));
		// Real loss of further video progress must still hold audio and report it.
		fixture.repeat_publication.set();
		fixture.release_publication.set();
		const uint64_t repeat_deadline = OS::get_singleton()->get_ticks_usec() + 110000;
		while (OS::get_singleton()->get_ticks_usec() < repeat_deadline) {
			playback->update(0.0);
			MessageQueue::get_singleton()->flush();
			OS::get_singleton()->delay_usec(1000);
		}
		playback->set_audio_playback_position(0.45);
		playback->update(0.0);
		MessageQueue::get_singleton()->flush();
		CHECK(sink.frames == int64_t(0.4 * fixture.mix_rate));
		CHECK(bool(playback->get("buffering")));
		fixture.release_publication.set();
		playback->stop();
		playback.unref();
		CHECK(fixture.owners.get() == 0);
	}
}

TEST_CASE("[SceneTree][NativeStreaming][PacketStarvation] Accepted real PCM prevents false starvation until its audible tail is exhausted") {
	initialize_audio();
	FiniteVideoFixture fixture;
	fixture.live = false;
	fixture.cpu_frames = true;
	fixture.prepared_cpu_frame = true;
	fixture.hold_video = true;
	fixture.presentable_position = 2.0;
	fixture.sample_value = 0.0f; // Decoded silence is real PCM, not underrun padding.
	fixture.mix_rate = AudioServer::get_singleton()->get_mix_rate();
	fixture.image = Image::create_empty(2, 2, false, Image::FORMAT_RGBA8);
	Ref<VideoStreamPlayback> playback = ffmpeg_test_async_video(FiniteVideoPlayback::create, &fixture);
	REQUIRE(playback.is_valid());
	AudioSink sink;
	playback->set_mix_callback(AudioSink::mix, &sink);
	playback->set_audio_playback_position(0.0);
	playback->set_audio_output_latency(0.0);
	playback->play();
	REQUIRE(wait_ready(playback, 2000000));
	const uint64_t accepted_deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
	while (sink.frames < fixture.mix_rate && OS::get_singleton()->get_ticks_usec() < accepted_deadline) {
		sink.live_available_frames = fixture.mix_rate;
		playback->update(0.02);
		MessageQueue::get_singleton()->flush();
		OS::get_singleton()->delay_usec(1000);
	}
	REQUIRE(sink.frames == fixture.mix_rate);
	CHECK(sink.energy == 0.0);
	fixture.packet_waiting.set();
	const uint64_t observed_deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
	while (!fixture.packet_wait_observed.is_set() && OS::get_singleton()->get_ticks_usec() < observed_deadline) {
		OS::get_singleton()->delay_usec(1000);
	}
	REQUIRE(fixture.packet_wait_observed.is_set());
	playback->set_audio_playback_position(0.5);
	sink.live_available_frames = 0;
	playback->update(0.02);
	MessageQueue::get_singleton()->flush();
	CHECK_FALSE(bool(playback->get("buffering")));
	playback->set_audio_playback_position(1.0 - 0.5 / fixture.mix_rate);
	playback->update(0.02);
	MessageQueue::get_singleton()->flush();
	CHECK(bool(playback->get("buffering")));
	fixture.audio_limit.set(fixture.mix_rate * 2);
	const uint64_t refilled_deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
	while (bool(playback->get("buffering")) && OS::get_singleton()->get_ticks_usec() < refilled_deadline) {
		playback->update(0.02);
		MessageQueue::get_singleton()->flush();
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK_FALSE(bool(playback->get("buffering")));
	CHECK(sink.frames == fixture.mix_rate);
	playback->stop();
	playback.unref();
	CHECK(fixture.owners.get() == 0);
}

TEST_CASE("[SceneTree][NativeStreaming][FiniteBackpressure] Slow finite video bounds paired audio and retains the complete terminal tail") {
	initialize_audio();
	FiniteVideoFixture fixture;
	fixture.live = false;
	fixture.cpu_frames = true;
	fixture.prepared_cpu_frame = true;
	fixture.hold_video = true;
	fixture.mix_rate = AudioServer::get_singleton()->get_mix_rate();
	fixture.image = Image::create_empty(2, 2, false, Image::FORMAT_RGBA8);
	fixture.image->fill(Color(0.3, 0.5, 0.7));
	Ref<VideoStreamPlayback> playback = ffmpeg_test_async_video(FiniteVideoPlayback::create, &fixture);
	REQUIRE(playback.is_valid());
	AudioSink sink;
	sink.live_available_frames = 0;
	playback->set_mix_callback(AudioSink::mix, &sink);
	playback->set_audio_playback_position(0.0);
	playback->set_audio_output_latency(0.0);
	playback->play();
	REQUIRE(wait_ready(playback, 2000000));
	const uint64_t fill_deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
	while (!fixture.filled.is_set() && OS::get_singleton()->get_ticks_usec() < fill_deadline) {
		OS::get_singleton()->delay_usec(1000);
	}
	REQUIRE(fixture.filled.is_set());
	for (int i = 0; i < 40; ++i) {
		sink.live_available_frames = 1024;
		playback->set_audio_playback_position(double(sink.frames) / fixture.mix_rate);
		playback->update(0.02);
		MessageQueue::get_singleton()->flush();
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(sink.frames >= int64_t(0.29 * fixture.mix_rate));
	CHECK(sink.frames <= int64_t((0.2 + 0.1) * fixture.mix_rate));
	CHECK(playback->is_playing());
	const uint64_t buffering_deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
	while (!bool(playback->get("buffering")) && OS::get_singleton()->get_ticks_usec() < buffering_deadline) {
		sink.live_available_frames = 1024;
		playback->set_audio_playback_position(double(sink.frames) / fixture.mix_rate);
		playback->update(0.02);
		MessageQueue::get_singleton()->flush();
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(bool(playback->get("buffering")));
	CHECK(sink.frames <= int64_t((0.2 + 0.1) * fixture.mix_rate));
	CHECK(int(playback->get("stream_error")) == OK);
	const int64_t held = sink.frames;
	playback->set_paused(true);
	sink.live_available_frames = fixture.mix_rate;
	playback->update(1.0);
	CHECK(sink.frames == held);
	playback->set_paused(false);
	fixture.release_video.set();
	const uint64_t end_deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
	while (playback->is_playing() && OS::get_singleton()->get_ticks_usec() < end_deadline) {
		sink.live_available_frames = 4096;
		playback->set_audio_playback_position(double(sink.frames) / fixture.mix_rate);
		playback->update(0.02);
		MessageQueue::get_singleton()->flush();
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK_FALSE(playback->is_playing());
	CHECK_FALSE(bool(playback->get("buffering")));
	CHECK(sink.frames == fixture.mix_rate);
	CHECK(sink.energy > 0.0);
	CHECK(fixture.owners.get() == 0);
}

TEST_CASE("[SceneTree][NativeStreaming][RecoveryTerminal] Terminal live video releases its input and cannot strand PCM beyond the final frame") {
	initialize_audio();
	for (bool live : { true, false }) {
		for (Error terminal_error : { OK, ERR_UNAVAILABLE }) {
			INFO(live, terminal_error);
			FiniteVideoFixture fixture;
			fixture.live = live;
			fixture.terminal_error = terminal_error;
			fixture.mix_rate = AudioServer::get_singleton()->get_mix_rate();
			Ref<VideoStreamPlayback> playback = ffmpeg_test_async_video(FiniteVideoPlayback::create, &fixture);
			REQUIRE(playback.is_valid());
			AudioSink sink;
			sink.live_available_frames = 0;
			playback->set_mix_callback(AudioSink::mix, &sink);
			playback->set_audio_playback_position(0.0);
			playback->set_audio_output_latency(0.0);
			SIGNAL_WATCH(playback.ptr(), "error");
			playback->play();
			REQUIRE(wait_ready(playback, 2000000));
			playback->update(0.1);
			const uint64_t produced_deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
			while (!fixture.produced.is_set() && OS::get_singleton()->get_ticks_usec() < produced_deadline) {
				OS::get_singleton()->delay_usec(1000);
			}
			REQUIRE(fixture.produced.is_set());
			// Let the worker publish terminal state, with frontend consumption
			// still blocked. Input ownership must not depend on queue draining.
			const uint64_t released_deadline = OS::get_singleton()->get_ticks_usec() + 250000;
			while (fixture.owners.get() > 0 && OS::get_singleton()->get_ticks_usec() < released_deadline) {
				OS::get_singleton()->delay_usec(1000);
			}
			CHECK(fixture.owners.get() == 0);
			if (terminal_error == OK) {
				CHECK(playback->is_playing()); // Playable queued PCM remains.
				playback->set_paused(true);
				sink.live_available_frames = 1024;
				playback->update(1.0);
				CHECK(sink.frames == 0);
				CHECK(playback->is_playing());
				playback->set_paused(false);
			}
			// Partial frontend acceptance must retain the final playable tail.
			sink.live_available_frames = 100;
			playback->update(0.0);
			if (terminal_error == OK) {
				CHECK(sink.frames == 100);
				CHECK(playback->is_playing());
			}
			const uint64_t end_deadline = OS::get_singleton()->get_ticks_usec() + 250000;
			while (playback->is_playing() && OS::get_singleton()->get_ticks_usec() < end_deadline) {
				sink.live_available_frames = 4096;
				playback->set_audio_playback_position(double(sink.frames) / fixture.mix_rate);
				playback->update(0.0);
				MessageQueue::get_singleton()->flush();
				OS::get_singleton()->delay_usec(1000);
			}
			MessageQueue::get_singleton()->flush();
			CHECK_FALSE(playback->is_playing());
			CHECK_FALSE(bool(playback->get("buffering")));
			CHECK(int(playback->get("stream_error")) == terminal_error);
			if (terminal_error == OK) {
				const int expected = live ? int((0.2 + 0.1) * fixture.mix_rate) : fixture.mix_rate;
				CHECK(sink.frames == expected);
				CHECK(sink.energy > 0.0);
				SIGNAL_CHECK_FALSE("error");
			} else {
				CHECK(sink.frames == 0);
				Array event;
				event.append(int(ERR_UNAVAILABLE));
				event.append("Video stream reading/decoding failed.");
				Array events;
				events.append(event);
				SIGNAL_CHECK("error", events);
			}
			SIGNAL_UNWATCH(playback.ptr(), "error");
			playback.unref();
			CHECK(fixture.owners.get() == 0);
		}
	}
}

class FiniteVideoStream : public VideoStream {
	GDCLASS(FiniteVideoStream, VideoStream);

protected:
	static void _bind_methods() {}

public:
	FiniteVideoFixture *fixture = nullptr;
	Ref<VideoStreamPlayback> instantiate_playback() override {
		return ffmpeg_test_async_video(FiniteVideoPlayback::create, fixture);
	}
};

struct ManualAudioDriver {
	AudioDriverDummy *driver = AudioDriverDummy::get_dummy_singleton();
	ManualAudioDriver() {
		driver->set_use_threads(false);
		initialize_audio();
	}
	~ManualAudioDriver() {
		// Restore the driver's normal mode even when REQUIRE aborts a test.
		if (AudioServer::get_singleton()) {
			AudioServer::get_singleton()->finish();
			memdelete(AudioServer::get_singleton());
		}
		driver->set_use_threads(true);
	}
};

struct VideoPlayerOwner {
	VideoStreamPlayer *player = memnew(VideoStreamPlayer);
	~VideoPlayerOwner() { memdelete(player); }
};

class PauseOnVideoTexture : public RefCounted {
	GDCLASS(PauseOnVideoTexture, RefCounted);

protected:
	static void _bind_methods() {}

public:
	VideoStreamPlayer *player = nullptr;
	bool called = false;
	double position = -1.0;
	void changed() {
		if (!called) {
			called = true;
			position = player->get_stream_position();
			player->set_paused(true);
		}
	}
};

TEST_CASE("[SceneTree][NativeStreaming] Publishing a CPU video frame preserves a pause requested by its texture callback") {
	REQUIRE(AudioServer::get_singleton() == nullptr);
	ManualAudioDriver audio;
	FiniteVideoFixture fixture;
	fixture.live = false;
	fixture.cpu_frames = true;
	fixture.mix_rate = AudioServer::get_singleton()->get_mix_rate();
	fixture.image = Image::create_empty(16, 16, false, Image::FORMAT_RGBA8);
	Ref<FiniteVideoStream> stream;
	stream.instantiate();
	stream->fixture = &fixture;
	VideoPlayerOwner owner;
	VideoStreamPlayer *player = owner.player;
	SceneTree::get_singleton()->get_root()->add_child(player);
	player->set_stream(stream);
	Ref<Texture2D> texture = player->get_video_texture();
	REQUIRE(texture.is_valid());
	Ref<PauseOnVideoTexture> recorder;
	recorder.instantiate();
	recorder->player = player;
	texture->connect(SNAME("changed"), callable_mp(recorder.ptr(), &PauseOnVideoTexture::changed));
	player->play();
	REQUIRE(wait_ready(player->get_stream_playback(), 2000000));
	int32_t output[512] = {};
	const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
	while (!recorder->called && OS::get_singleton()->get_ticks_usec() < deadline) {
		player->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
		audio.driver->mix_audio(256, output);
		MessageQueue::get_singleton()->flush();
		OS::get_singleton()->delay_usec(1000);
	}
	REQUIRE(recorder->called);
	CHECK(player->is_paused());
	CHECK(Math::is_equal_approx(player->get_stream_position(), recorder->position));
	CHECK(Math::is_equal_approx(recorder->position, 0.2));
	CHECK(texture->get_width() == 16);
	CHECK(texture->get_height() == 16);
	texture->disconnect(SNAME("changed"), callable_mp(recorder.ptr(), &PauseOnVideoTexture::changed));
	player->stop();
}

TEST_CASE("[SceneTree][NativeStreaming] Readiness publishes a prepared frame even while paused without advancing media") {
	REQUIRE(AudioServer::get_singleton() == nullptr);
	ManualAudioDriver audio;
	FiniteVideoFixture fixture;
	fixture.live = false;
	fixture.cpu_frames = true;
	fixture.prepared_cpu_frame = true;
	fixture.mix_rate = AudioServer::get_singleton()->get_mix_rate();
	fixture.image = Image::create_empty(16, 16, false, Image::FORMAT_RGBA8);
	Ref<FiniteVideoStream> stream;
	stream.instantiate();
	stream->fixture = &fixture;
	VideoPlayerOwner owner;
	VideoStreamPlayer *player = owner.player;
	SceneTree::get_singleton()->get_root()->add_child(player);
	player->set_stream(stream);
	player->set_paused(true);
	player->play();
	REQUIRE(wait_ready(player->get_stream_playback(), 2000000));
	const uint64_t publish_deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
	while (player->get_video_texture()->get_width() == 0 && OS::get_singleton()->get_ticks_usec() < publish_deadline) {
		MessageQueue::get_singleton()->flush();
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(player->is_paused());
	CHECK(player->get_video_texture()->get_width() == 16);
	CHECK(player->get_video_texture()->get_height() == 16);
	CHECK(Math::is_equal_approx(player->get_stream_position(), 0.2));
	player->get_stream_playback()->update(1.0);
	CHECK(Math::is_equal_approx(player->get_stream_position(), 0.2));
	CHECK_FALSE(fixture.produced.is_set());
	player->stop();
}

TEST_CASE("[SceneTree][NativeStreaming][RecoveryTerminal] Native player observes asynchronous EOF once and delivers its accepted audio tail") {
	REQUIRE(AudioServer::get_singleton() == nullptr);
	ManualAudioDriver audio;
	REQUIRE(audio.driver->get_channels() == 2);
	for (bool live : { false, true }) {
		for (bool empty : { false, true }) {
			INFO(live, empty);
			FiniteVideoFixture fixture;
			fixture.live = live;
			fixture.empty = empty;
			fixture.mix_rate = AudioServer::get_singleton()->get_mix_rate();
			Ref<FiniteVideoStream> stream;
			stream.instantiate();
			stream->fixture = &fixture;
			VideoPlayerOwner owner;
			VideoStreamPlayer *player = owner.player;
			SceneTree::get_singleton()->get_root()->add_child(player);
			player->set_stream(stream);
			Ref<VideoStreamPlayback> playback = player->get_stream_playback();
			REQUIRE(playback.is_valid());
			SIGNAL_WATCH(player, "finished");
			player->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
			SIGNAL_CHECK_FALSE("finished"); // Prepared does not mean started.
			player->play();
			REQUIRE(wait_ready(playback, 2000000));
			playback->set_audio_output_latency(0.0);
			playback->set_audio_playback_position(0.0);
			playback->update(0.1);
			const uint64_t produced_deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
			while ((!fixture.produced.is_set() || fixture.owners.get() > 0) && OS::get_singleton()->get_ticks_usec() < produced_deadline) {
				OS::get_singleton()->delay_usec(1000);
			}
			REQUIRE(fixture.produced.is_set());
			CHECK(fixture.owners.get() == 0);
			// Fill the frontend before deliberately repeating completion signals
			// for this same generation. They must not flush accepted samples or
			// reset the consumed clock a second time.
			if (!empty) {
				player->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
				playback->emit_signal(SNAME("ready"));
				playback->emit_signal(SNAME("seek_completed"), playback->get_playback_position());
			}
			int32_t output[512] = {};
			player->set_paused(true);
			player->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
			audio.driver->mix_audio(256, output);
			int paused_nonzero = 0;
			for (int32_t sample : output) {
				paused_nonzero += sample != 0;
			}
			CHECK(paused_nonzero == 0);
			SIGNAL_CHECK_FALSE("finished");
			player->set_paused(false);
			int nonzero_frames = 0;
			const uint64_t end_deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
			while (player->is_processing_internal() && OS::get_singleton()->get_ticks_usec() < end_deadline) {
				player->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
				audio.driver->mix_audio(256, output);
				for (int i = 0; i < 256; i++) {
					nonzero_frames += output[i * 2] != 0 || output[i * 2 + 1] != 0;
				}
				MessageQueue::get_singleton()->flush();
				OS::get_singleton()->delay_usec(1000);
			}
			CHECK_FALSE(player->is_playing());
			CHECK_FALSE(player->is_processing_internal());
			CHECK(int(playback->get("stream_error")) == OK);
			Array finished;
			finished.append(Array());
			SIGNAL_CHECK("finished", finished);
			const int expected = empty ? 0 : int((live ? 0.3 : 1.0) * fixture.mix_rate);
			if (empty) {
				CHECK(nonzero_frames == 0);
			} else {
				// One mixer block covers interpolation/end-fade rounding. Losing
				// a queued frontend tail would drop many blocks, not this margin.
				CHECK(nonzero_frames >= expected - 256);
				CHECK(nonzero_frames <= expected + 256);
			}
			player->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
			SIGNAL_CHECK_FALSE("finished");
			player->play();
			player->stop();
			player->notification(Node::NOTIFICATION_INTERNAL_PROCESS);
			SIGNAL_CHECK_FALSE("finished");
			SIGNAL_UNWATCH(player, "finished");
		}
	}
}

TEST_CASE("[SceneTree][NativeStreaming] Invalid prepared video frames release their opened input before reporting failure") {
	REQUIRE(AudioServer::get_singleton() == nullptr);
	ManualAudioDriver audio;
	FiniteVideoFixture fixture;
	fixture.cpu_frames = true;
	fixture.prepared_cpu_frame = true;
	fixture.mix_rate = AudioServer::get_singleton()->get_mix_rate();
	fixture.image.instantiate(); // An invalid empty image cannot enter the queue.
	Ref<VideoStreamPlayback> playback = ffmpeg_test_async_video(FiniteVideoPlayback::create, &fixture);
	REQUIRE(playback.is_valid());
	playback->play();
	CHECK_FALSE(wait_ready(playback, 2000000));
	CHECK(int(playback->get("stream_error")) == ERR_CANT_OPEN);
	const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 250000;
	while (fixture.owners.get() > 0 && OS::get_singleton()->get_ticks_usec() < deadline) {
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(fixture.owners.get() == 0);
	CHECK_FALSE(playback->is_playing());
}

struct PacketReadFixture {
	SafeNumeric<int> calls;
	SafeNumeric<int> allowed{ INT32_MAX };
	SafeFlag cancel_observed;
	int total = INT32_MAX;
	int payload = 16;
	int terminal = AVERROR_EOF;
	bool alternating_streams = false;
	bool timestamps = true;
	bool large_side_data = false;
	static int read(void *p_self, AVPacket *p_packet, const SafeFlag *p_stop, const SafeFlag *p_external) {
		PacketReadFixture *self = static_cast<PacketReadFixture *>(p_self);
		const int sequence = self->calls.increment() - 1;
		if (sequence >= self->total) {
			return self->terminal;
		}
		while (sequence >= self->allowed.get()) {
			if (p_stop->is_set() || (p_external && p_external->is_set())) {
				self->cancel_observed.set();
				return AVERROR_EXIT;
			}
			OS::get_singleton()->delay_usec(1000);
		}
		if (av_new_packet(p_packet, self->payload) < 0) {
			return AVERROR(ENOMEM);
		}
		memset(p_packet->data, 0x20, self->payload);
		p_packet->stream_index = self->alternating_streams ? sequence % 2 : 0;
		p_packet->pts = p_packet->dts = self->timestamps ? sequence * 1024 : AV_NOPTS_VALUE;
		p_packet->duration = 1024;
		if (self->large_side_data && !av_packet_new_side_data(p_packet, AV_PKT_DATA_NEW_EXTRADATA, 8 * 1024 * 1024)) {
			return AVERROR(ENOMEM);
		}
		return 0;
	}
};

struct PacketOwner {
	AVPacket *packet = av_packet_alloc();
	~PacketOwner() { av_packet_free(&packet); }
};

static bool wait_packet_condition(const FFmpegPacketReader &p_reader, bool p_terminal) {
	const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
	while (OS::get_singleton()->get_ticks_usec() < deadline) {
		if (p_terminal ? p_reader.get_terminal_error() < 0 : p_reader.has_prefill(1000000000.0)) {
			return true;
		}
		OS::get_singleton()->delay_usec(1000);
	}
	return false;
}

static bool wait_packet_calls(const PacketReadFixture &p_fixture, int p_count) {
	const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
	while (p_fixture.calls.get() < p_count && OS::get_singleton()->get_ticks_usec() < deadline) {
		OS::get_singleton()->delay_usec(1000);
	}
	return p_fixture.calls.get() >= p_count;
}

struct GracefulPacketStop {
	FFmpegPacketReader *reader = nullptr;
	static void run(void *p_self) {
		static_cast<GracefulPacketStop *>(p_self)->reader->stop(false);
	}
};

TEST_CASE("[NativeStreaming][PacketReadAhead] Finite seek joins an intact read and external cancellation still interrupts") {
	for (bool cancel : { false, true }) {
		PacketReadFixture fixture;
		fixture.allowed.set(0);
		SafeFlag external;
		FFmpegPacketReader reader;
		REQUIRE(reader.start_for_test(PacketReadFixture::read, &fixture, 0, { 1, 1024 }, &external) == OK);
		REQUIRE(wait_packet_calls(fixture, 1));
		GracefulPacketStop stop{ &reader };
		Thread joiner;
		joiner.start(GracefulPacketStop::run, &stop);
		OS::get_singleton()->delay_usec(20000);
		CHECK_FALSE(reader.get_stop_flag()->is_set());
		if (cancel) {
			external.set();
		} else {
			fixture.allowed.set(1);
		}
		joiner.wait_to_finish();
		CHECK(fixture.cancel_observed.is_set() == cancel);
		CHECK_FALSE(reader.is_started());
	}
}

TEST_CASE("[NativeStreaming][PacketReadAhead] Selected packets retain FIFO order before terminal status") {
	for (int terminal : { AVERROR_EOF, AVERROR(EIO) }) {
		PacketReadFixture fixture;
		fixture.total = 6;
		fixture.terminal = terminal;
		fixture.alternating_streams = true;
		FFmpegPacketReader reader;
		PacketOwner packet;
		REQUIRE(packet.packet);
		REQUIRE(reader.start_for_test(PacketReadFixture::read, &fixture, 1, { 1, 1024 }) == OK);
		REQUIRE(wait_packet_condition(reader, true));
		CHECK(reader.has_prefill());
		for (int i = 0; i < 3; i++) {
			CHECK(reader.read(packet.packet) == 0);
			CHECK(packet.packet->stream_index == 1);
			CHECK(packet.packet->pts == (i * 2 + 1) * 1024);
		}
		CHECK(reader.read(packet.packet) == terminal);
		CHECK(reader.get_terminal_error() == terminal);
	}
}

TEST_CASE("[NativeStreaming][PacketReadAhead] Packet and byte limits backpressure producers and wake on stop") {
	for (int payload : { 1, 1024 * 1024 }) {
		PacketReadFixture fixture;
		fixture.payload = payload;
		fixture.timestamps = false;
		FFmpegPacketReader reader;
		PacketOwner packet;
		REQUIRE(packet.packet);
		REQUIRE(reader.start_for_test(PacketReadFixture::read, &fixture, 0, { 1, 1024 }) == OK);
		REQUIRE(wait_packet_condition(reader, false));
		const int produced = fixture.calls.get();
		CHECK(produced > 1);
		CHECK(produced <= (payload == 1 ? 1025 : 9));
		OS::get_singleton()->delay_usec(20000);
		CHECK(fixture.calls.get() == produced);
		const uint64_t start = OS::get_singleton()->get_ticks_usec();
		reader.stop();
		CHECK(OS::get_singleton()->get_ticks_usec() - start < 250000);
		CHECK_FALSE(reader.is_started());
		CHECK(reader.read(packet.packet) == AVERROR_EXIT);
	}
}

TEST_CASE("[NativeStreaming][PacketReadAhead] Oversized packet and side data allocations fail explicitly") {
	for (bool side_data : { false, true }) {
		PacketReadFixture fixture;
		fixture.payload = side_data ? 16 : 8 * 1024 * 1024 + 1;
		fixture.large_side_data = side_data;
		FFmpegPacketReader reader;
		PacketOwner packet;
		REQUIRE(packet.packet);
		REQUIRE(reader.start_for_test(PacketReadFixture::read, &fixture, 0, { 1, 1024 }) == OK);
		REQUIRE(wait_packet_condition(reader, true));
		CHECK(reader.get_terminal_error() == AVERROR(ENOBUFS));
		CHECK_FALSE(reader.has_prefill());
		CHECK(reader.read(packet.packet) == AVERROR(ENOBUFS));
		CHECK(fixture.calls.get() == 1);
	}
}

TEST_CASE("[NativeStreaming][PacketReadAhead] Empty reads stay nonblocking and cancellation reaches blocked producers") {
	for (bool external : { false, true }) {
		PacketReadFixture fixture;
		fixture.allowed.set(0);
		SafeFlag generation_cancel;
		FFmpegPacketReader reader;
		PacketOwner packet;
		REQUIRE(packet.packet);
		REQUIRE(reader.start_for_test(PacketReadFixture::read, &fixture, 0, { 1, 1024 }, &generation_cancel) == OK);
		REQUIRE(wait_packet_calls(fixture, 1));
		uint64_t start = OS::get_singleton()->get_ticks_usec();
		CHECK(reader.read(packet.packet) == AVERROR(EAGAIN));
		CHECK(OS::get_singleton()->get_ticks_usec() - start < 25000);
		start = OS::get_singleton()->get_ticks_usec();
		if (external) {
			generation_cancel.set();
			CHECK(wait_packet_condition(reader, true));
		}
		reader.stop();
		CHECK(OS::get_singleton()->get_ticks_usec() - start < 250000);
		CHECK(fixture.cancel_observed.is_set());
		CHECK_FALSE(reader.is_started());
	}
	SafeFlag generation_cancel;
	SafeFlag reader_stop;
	reader_stop.set();
	MediaHTTP cancelled_http(&generation_cancel, &reader_stop);
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	CHECK(cancelled_http.open("https://example.com/no-network-request") == ERR_TIMEOUT);
	CHECK(OS::get_singleton()->get_ticks_usec() - start < 25000);
}

static Vector<uint8_t> packet_test_wave(int p_rate, int p_frames) {
	Vector<uint8_t> wave;
	wave.resize(44 + p_frames * 4);
	uint8_t *data = wave.ptrw();
	memset(data, 0x20, wave.size());
	auto put16 = [data](int at, uint16_t value) {
		data[at] = value & 255;
		data[at + 1] = value >> 8;
	};
	auto put32 = [data](int at, uint32_t value) {
		for (int i = 0; i < 4; i++) {
			data[at + i] = (value >> (i * 8)) & 255;
		}
	};
	memcpy(data, "RIFF", 4);
	put32(4, wave.size() - 8);
	memcpy(data + 8, "WAVEfmt ", 8);
	put32(16, 16);
	put16(20, 1);
	put16(22, 2);
	put32(24, p_rate);
	put32(28, p_rate * 4);
	put16(32, 4);
	put16(34, 16);
	memcpy(data + 36, "data", 4);
	put32(40, wave.size() - 44);
	return wave;
}

struct MediaPreparationOwner {
	MediaStreamPlayer *player = memnew(MediaStreamPlayer);
	~MediaPreparationOwner() { memdelete(player); }
};

static void preparation_frame() {
	SceneTree::get_singleton()->emit_signal(SNAME("process_frame"));
	MessageQueue::get_singleton()->flush();
	OS::get_singleton()->delay_usec(1000);
}

TEST_CASE("[SceneTree][NativeStreaming][AudioShutdown] Finishing audio releases stopped playback without another mix") {
	ObjectID wave_id;
	ObjectID playback_id;
	{
		ManualAudioDriver audio;
		Ref<AudioStreamWAV> wave;
		wave.instantiate();
		wave->set_format(AudioStreamWAV::FORMAT_16_BITS);
		wave->set_mix_rate(48000);
		wave->set_data(packet_test_wave(48000, 96000).slice(44));
		wave_id = wave->get_instance_id();
		AudioStreamPlayer *player = memnew(AudioStreamPlayer);
		SceneTree::get_singleton()->get_root()->add_child(player);
		player->set_stream(wave);
		player->play();
		Ref<AudioStreamPlayback> playback = player->get_stream_playback();
		REQUIRE(playback.is_valid());
		playback_id = playback->get_instance_id();
		int32_t output[512] = {};
		audio.driver->mix_audio(256, output);
		AudioServer::get_singleton()->update();
		player->set_stream_paused(true);
		player->stop();
		memdelete(player);
		playback.unref();
		wave.unref();
		// The driver owns the last references until its next mix, or finish().
		CHECK(ObjectDB::get_instance(wave_id) != nullptr);
		CHECK(ObjectDB::get_instance(playback_id) != nullptr);
		// ManualAudioDriver finishes and destroys the server on scope exit.
		// Deliberately do not mix or update again to retire the playback.
	}
	CHECK(ObjectDB::get_instance(wave_id) == nullptr);
	CHECK(ObjectDB::get_instance(playback_id) == nullptr);
}

TEST_CASE("[SceneTree][NativeStreaming][PausePosition] Pausing refreshes a snapshot that missed audio mixing during a UI stall") {
	ManualAudioDriver audio;
	Ref<FileAccess> file = FileAccess::create_temp(FileAccess::READ_WRITE, "godot-pause-position", "wav");
	REQUIRE(file.is_valid());
	file->store_buffer(packet_test_wave(48000, 96000));
	file->flush();
	MediaPreparationOwner owner;
	MediaStreamPlayer *player = owner.player;
	SceneTree::get_singleton()->get_root()->add_child(player);
	Dictionary options;
	options["prepare_pcm"] = true;
	options["autoplay"] = true;
	options["allow_browser_fallback"] = false;
	REQUIRE(player->open(file->get_path_absolute(), options) == OK);
	const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
	while (player->is_preparing() && OS::get_singleton()->get_ticks_usec() < deadline) {
		preparation_frame();
	}
	REQUIRE(bool(player->get_snapshot()["ready"]));
	player->set_volume_linear(0.37);
	const int64_t generation = player->get_generation();
	const double cached = player->get_snapshot()["position"];
	AudioStreamPlayer *stream_player = nullptr;
	for (int i = 0; i < player->get_child_count(true); i++) {
		if (AudioStreamPlayer *child = Object::cast_to<AudioStreamPlayer>(player->get_child(i, true))) {
			stream_player = child;
		}
	}
	REQUIRE(stream_player);
	// Mix real audio without a controller process notification, as happens
	// while the main thread is stalled. No sockets or wall-clock sleep needed.
	int32_t output[512] = {};
	for (int i = 0; i < 48; i++) {
		audio.driver->mix_audio(256, output);
		AudioServer::get_singleton()->update();
	}
	const double actual = stream_player->get_playback_position();
	REQUIRE(actual > cached + 0.08);
	CHECK(double(player->get_snapshot()["position"]) == cached);
	player->set_paused(true);
	CHECK(Math::abs(double(player->get_snapshot()["position"]) - actual) < 0.002);
	CHECK(player->is_paused());
	CHECK(player->get_generation() == generation);
	CHECK(Math::abs(player->get_volume_linear() - 0.37) < 0.001);
	player->stop();
	for (int i = 0; i < 4; i++) {
		audio.driver->mix_audio(256, output);
		AudioServer::get_singleton()->update();
	}
}

TEST_CASE("[SceneTree][NativeStreaming][BackendRetirement] Stopped native playback retains an ownership fence until its last reference retires") {
	ManualAudioDriver audio;
	TestMediaHTTP::ScriptedHTTP fixture;
	TestMediaHTTP::ScriptedReply reply;
	// The real native worker/demuxer uses the scripted HTTP factory, not sockets.
	reply.binary_body = packet_test_wave(48000);
	reply.length = reply.binary_body.size();
	fixture.replies.push_back(reply);
	TestMediaHTTP::ScriptedFactory factory(fixture);
	MediaPreparationOwner owner;
	MediaStreamPlayer *player = owner.player;
	SceneTree::get_singleton()->get_root()->add_child(player);
	Dictionary options;
	options["autoplay"] = false;
	REQUIRE(player->open("https://8.8.8.8/fixture.wav", options) == OK);
	Ref<AudioStreamPlayback> retained;
	for (int i = 0; i < player->get_child_count(true); i++) {
		AudioStreamPlayer *child = Object::cast_to<AudioStreamPlayer>(player->get_child(i, true));
		if (child && child->has_stream_playback()) {
			retained = child->get_stream_playback();
		}
	}
	REQUIRE(retained.is_valid());
	const int64_t opening = player->get_generation();
	const int64_t backend_id = int64_t(uint64_t(retained->get_instance_id()));
	CHECK_FALSE(player->is_backend_retiring());
	const uint64_t began = OS::get_singleton()->get_ticks_usec();
	player->stop();
	CHECK(OS::get_singleton()->get_ticks_usec() - began < 250000);
	CHECK_FALSE(player->is_preparing());
	REQUIRE(player->is_backend_retiring());
	Dictionary state = player->get_backend_retirement_state();
	REQUIRE(int(state["retiring_count"]) == 1);
	Dictionary identity = Array(state["retiring"])[0];
	CHECK(int64_t(identity["generation"]) == opening);
	CHECK(int64_t(identity["backend_instance_id"]) == backend_id);
	CHECK(String(identity["source"]) == "https://8.8.8.8/fixture.wav");
	int32_t output[512] = {};
	for (int i = 0; i < 4; i++) {
		audio.driver->mix_audio(256, output);
		AudioServer::get_singleton()->update();
		preparation_frame();
	}
	CHECK(player->is_backend_retiring()); // External ownership is still real.
	retained.unref();
	const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
	while (player->is_backend_retiring() && OS::get_singleton()->get_ticks_usec() < deadline) {
		audio.driver->mix_audio(256, output);
		AudioServer::get_singleton()->update();
		preparation_frame();
	}
	CHECK_FALSE(player->is_backend_retiring());
	CHECK(int(player->get_backend_retirement_state()["retiring_count"]) == 0);
}

TEST_CASE("[SceneTree][NativeStreaming][PCMPreparation] Canceled PCM retains its identity and rejects replacement until frame collection") {
	ManualAudioDriver audio;
	Ref<FileAccess> file = FileAccess::create_temp(FileAccess::READ_WRITE, "godot-preparation", "wav");
	REQUIRE(file.is_valid());
	file->store_buffer(packet_test_wave(48000));
	file->flush();
	MediaPreparationOwner owner;
	MediaStreamPlayer *player = owner.player;
	SceneTree::get_singleton()->get_root()->add_child(player);
	Dictionary options;
	options["prepare_pcm"] = true;
	options["autoplay"] = false;
	options["allow_browser_fallback"] = false;
	options["mix_rate"] = 48000;
	CHECK_FALSE(player->is_preparing());
	for (int cycle = 0; cycle < 8; cycle++) {
		REQUIRE(player->open(file->get_path_absolute(), options) == OK);
		Dictionary accepted = player->get_snapshot().duplicate(true);
		Dictionary preparation = accepted["preparation"];
		Dictionary active = preparation["active"];
		CHECK(player->is_preparing());
		CHECK(int(preparation["active_count"]) == 1);
		CHECK(int(preparation["retiring_count"]) == 0);
		CHECK(int64_t(active["generation"]) == player->get_generation());
		CHECK(String(active["source"]) == file->get_path_absolute());
		CHECK(int64_t(active["request_id"]) > 0);
		CHECK(player->open(file->get_path_absolute() + ".replacement", options) == ERR_BUSY);
		CHECK(player->get_snapshot() == accepted);
		player->stop();
		Dictionary stopped = player->get_snapshot().duplicate(true);
		preparation = stopped["preparation"];
		Array retiring = preparation["retiring"];
		CHECK(player->is_preparing());
		CHECK_FALSE(bool(stopped["ready"]));
		CHECK_FALSE(bool(stopped["buffering"]));
		CHECK(int(preparation["active_count"]) == 0);
		REQUIRE(retiring.size() == 1);
		CHECK(Dictionary(retiring[0]) == active);
		CHECK(player->open(file->get_path_absolute(), options) == ERR_BUSY);
		CHECK(player->get_snapshot() == stopped);
		// Collection belongs to the scene tree, even when this controller cannot
		// receive internal process notifications (e.g. paused UI).
		player->set_process_mode(Node::PROCESS_MODE_DISABLED);
		const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
		while (player->is_preparing() && OS::get_singleton()->get_ticks_usec() < deadline) {
			preparation_frame();
		}
		REQUIRE_FALSE(player->is_preparing());
		CHECK_FALSE(bool(player->get_snapshot()["ready"]));
		CHECK(Array(player->get_preparation_state()["retiring"]).is_empty());
		player->set_process_mode(Node::PROCESS_MODE_INHERIT);
	}
	REQUIRE(player->open(file->get_path_absolute(), options) == OK);
	const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
	while (player->is_preparing() && OS::get_singleton()->get_ticks_usec() < deadline) {
		preparation_frame();
	}
	REQUIRE_FALSE(player->is_preparing());
	CHECK(bool(player->get_snapshot()["ready"]));
	CHECK(bool(player->get_capabilities()["precise_loop"]));
	CHECK(player->set_loop_range(0.0123, 0.0321) == OK);
	player->stop();
	// This unit runner does not execute the normal audio/frame loop. Retire
	// stopped mixer references explicitly before destroying its AudioServer.
	int32_t output[512] = {};
	for (int i = 0; i < 4; i++) {
		audio.driver->mix_audio(256, output);
		AudioServer::get_singleton()->update();
	}
}

TEST_CASE("[SceneTree][NativeStreaming][PCMPreparation] Destroying a preparing controller cancels without losing worker lifetime") {
	initialize_audio();
	Ref<FileAccess> file = FileAccess::create_temp(FileAccess::READ_WRITE, "godot-preparation-owner", "wav");
	REQUIRE(file.is_valid());
	file->store_buffer(packet_test_wave(48000));
	file->flush();
	MediaStreamPlayer *player = memnew(MediaStreamPlayer);
	SceneTree::get_singleton()->get_root()->add_child(player);
	Dictionary options;
	options["prepare_pcm"] = true;
	options["mix_rate"] = 48000;
	REQUIRE(player->open(file->get_path_absolute(), options) == OK);
	Dictionary active = player->get_preparation_state()["active"];
	ObjectID decoder_id = ObjectID(uint64_t(int64_t(active["decoder_instance_id"])));
	memdelete(player);
	CHECK(ObjectDB::get_instance(decoder_id) != nullptr);
	const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
	while (ObjectDB::get_instance(decoder_id) && OS::get_singleton()->get_ticks_usec() < deadline) {
		preparation_frame();
	}
	CHECK(ObjectDB::get_instance(decoder_id) == nullptr);
}

TEST_CASE("[SceneTree][NativeStreaming][MediaMetadata] Direct media titles come from opened headers with a bounded filename fallback") {
	initialize_audio();
	Dictionary fallback = FFmpegMediaMetadata::source("https://example.com/audio/Queued%20Track.mp3?secret=not-a-title");
	CHECK(String(fallback["title"]) == "Queued Track.mp3");
	CHECK_FALSE(bool(fallback["opened"]));
	CHECK(FFmpegMediaMetadata::title(String("x").repeat(300)).length() == 256);
	CHECK(FFmpegMediaMetadata::title("\tA\nB\rC\x01") == "A B C");
	Vector<uint8_t> wave = packet_test_wave(44100);
	const CharString tagged = String("Direct media title").utf8();
	const int size = (tagged.length() + 2) & ~1;
	const int extra = 20 + size;
	Vector<uint8_t> with_tag;
	with_tag.resize(wave.size() + extra);
	uint8_t *data = with_tag.ptrw();
	memcpy(data, wave.ptr(), 36);
	memset(data + 36, 0, extra);
	memcpy(data + 36, "LIST", 4);
	memcpy(data + 44, "INFOINAM", 8);
	auto put32 = [data](int at, uint32_t value) {
		for (int i = 0; i < 4; i++) {
			data[at + i] = (value >> (8 * i)) & 255;
		}
	};
	put32(4, with_tag.size() - 8);
	put32(40, extra - 8);
	put32(52, size);
	memcpy(data + 56, tagged.get_data(), tagged.length());
	memcpy(data + 36 + extra, wave.ptr() + 36, wave.size() - 36);
	Ref<AudioStreamPlayback> playback = ffmpeg_audio_playback_from_buffer(with_tag);
	REQUIRE(playback.is_valid());
	Dictionary metadata = playback->get_parameter(SNAME("_ffmpeg_metadata"));
	CHECK(String(metadata["title"]) == "Direct media title");
	CHECK(String(metadata["title_source"]) == "container");
	CHECK(bool(metadata["opened"]));
	CHECK(double(metadata["duration"]) > 0);
	Dictionary youtube = fallback.duplicate();
	youtube["source_type"] = "youtube";
	youtube["title"] = "YouTube title from existing resolution";
	youtube["is_live"] = true;
	youtube["duration"] = 0.0;
	Dictionary merged = FFmpegMediaMetadata::opened(youtube, metadata);
	CHECK(String(merged["title"]) == String(youtube["title"]));
	CHECK(String(merged["source"]) == String(fallback["source"]));
	CHECK(String(merged["title_source"]) == "youtube");
	CHECK(double(merged["duration"]) == 0);
}

static Vector<Dictionary> metadata_events;
static void record_metadata(Dictionary p_metadata) {
	metadata_events.push_back(p_metadata);
}

TEST_CASE("[SceneTree][NativeStreaming][PacketReadAhead] Audio preserves playing and partial PCM across packet starvation") {
	initialize_audio();
	PacketReadFixture fixture;
	fixture.total = 2;
	fixture.payload = 1024 * 4;
	fixture.allowed.set(0);
	Ref<AudioStreamPlayback> playback = ffmpeg_test_audio_packet_reader(packet_test_wave(AudioServer::get_singleton()->get_mix_rate()), PacketReadFixture::read, &fixture);
	REQUIRE(playback.is_valid());
	playback->start();
	REQUIRE(wait_packet_calls(fixture, 1));
	AudioFrame samples[2048];
	CHECK(playback->mix(samples, 1.0, 2048) == 0);
	CHECK(playback->is_playing());
	CHECK(int(playback->get_parameter(SNAME("stream_error"))) == OK);
	fixture.allowed.set(1);
	REQUIRE(wait_packet_calls(fixture, 2));
	const int partial = playback->mix(samples, 1.0, 2048);
	CHECK(partial == 1024);
	CHECK(playback->is_playing());
	CHECK(int(playback->get_parameter(SNAME("stream_error"))) == OK);
	double energy = 0;
	for (int i = 0; i < partial; i++) {
		energy += Math::abs(samples[i].left) + Math::abs(samples[i].right);
	}
	CHECK(energy > 0);
	fixture.allowed.set(2);
	const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
	while (int(playback->get_parameter(SNAME("_ffmpeg_read_ahead_error"))) == 0 && OS::get_singleton()->get_ticks_usec() < deadline) {
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(int(playback->get_parameter(SNAME("_ffmpeg_read_ahead_error"))) == AVERROR_EOF);
	const int recovered = playback->mix(samples, 1.0, 2048);
	CHECK(recovered == 1024);
	CHECK_FALSE(playback->is_playing());
	CHECK(int(playback->get_parameter(SNAME("stream_error"))) == OK);
	energy = 0;
	for (int i = 0; i < recovered; i++) {
		energy += Math::abs(samples[i].left) + Math::abs(samples[i].right);
	}
	CHECK(energy > 0);
}

TEST_CASE("[SceneTree][NativeStreaming][SeekPreroll] A completed audio seek retains PCM when following packets are delayed") {
	initialize_audio();
	const int rate = AudioServer::get_singleton()->get_mix_rate();
	for (bool cancel : { false, true }) {
		SafeFlag cancelled;
		if (cancel) {
			cancelled.set();
		}
		PacketReadFixture fixture;
		fixture.allowed.set(0);
		const double from = 0.0125;
		Ref<AudioStreamPlayback> playback = ffmpeg_test_audio_packet_reader(packet_test_wave(rate), PacketReadFixture::read, &fixture, &cancelled, from);
		if (cancel) {
			CHECK(playback.is_null());
			continue;
		}
		REQUIRE(playback.is_valid());
		playback->start(from);
		REQUIRE(wait_packet_calls(fixture, 1));
		CHECK(Math::abs(playback->get_playback_position() - from) <= 1.0 / rate);
		AudioFrame samples[256];
		CHECK(playback->mix(samples, 1.0, 256) == 256);
		double energy = 0;
		for (const AudioFrame &sample : samples) {
			energy += Math::abs(sample.left) + Math::abs(sample.right);
		}
		CHECK(energy > 1.0);
		CHECK(playback->is_playing());
		CHECK(int(playback->get_parameter(SNAME("stream_error"))) == OK);
		CHECK(Math::abs(playback->get_playback_position() - from - 256.0 / rate) <= 1.0 / rate);
	}
}

static void check_playback(const String &url, bool p_explicit_pair) {
	REQUIRE(!url.is_empty());
	if (url.is_empty()) {
		return;
	}
	TestMediaStream::load_certificates();
	initialize_audio();
	Ref<VideoStreamMedia> stream;
	stream.instantiate();
	stream->set_file(url);
	stream->set_name("Authoritative project title");
	// Two independent network inputs from a known public test movie. The first
	// decoder selects video; the second decoder selects audio, as for split URLs.
	if (p_explicit_pair) {
		stream->set_audio_file(url);
	}
	uint64_t start = OS::get_singleton()->get_ticks_usec();
	Ref<VideoStreamPlayback> playback = stream->instantiate_playback();
	CHECK(OS::get_singleton()->get_ticks_usec() - start < 250000);
	REQUIRE(playback.is_valid());
	if (playback.is_null()) {
		return;
	}
	SIGNAL_WATCH(playback.ptr(), "ready");
	SIGNAL_WATCH(playback.ptr(), "seek_completed");
	metadata_events.clear();
	playback->connect("metadata_changed", callable_mp_static(record_metadata));
	AudioSink sink;
	playback->set_mix_callback(AudioSink::mix, &sink);
	bool ready = wait_ready(playback);
	CHECK(ready);
	if (ready) {
		Array expected;
		expected.append(Array());
		SIGNAL_CHECK("ready", expected);
		CHECK(playback->get_length() > 10);
		Dictionary metadata = playback->get_stream_metadata();
		CHECK(bool(metadata.get("opened", false)));
		CHECK(!String(metadata.get("title", "")).is_empty());
		CHECK(String(metadata["title"]).length() <= 256);
		CHECK(String(metadata["source"]) == url);
		CHECK(int64_t(metadata["playback_instance_id"]) == int64_t(uint64_t(playback->get_instance_id())));
		CHECK(stream->get_name() == "Authoritative project title");
		String resolved_title = metadata["title"];
		metadata["title"] = "Caller-owned override";
		CHECK(String(playback->get_stream_metadata()["title"]) == resolved_title);
		playback->play();
		CHECK(wait_ready(playback));
		uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 20000000;
		while (playback->get_playback_position() < 2.0 && OS::get_singleton()->get_ticks_usec() < deadline && playback->is_playing()) {
			playback->update(0.016);
			MessageQueue::get_singleton()->flush();
			OS::get_singleton()->delay_usec(16000);
		}
		playback->update(0);
		CHECK(playback->get_playback_position() >= 2.0);
		CHECK(sink.frames > 0);
		CHECK(sink.energy > 0);
		CHECK(Math::abs(double(sink.frames) / playback->get_mix_rate() - playback->get_playback_position()) < 0.3);
		CHECK(playback->get_texture().is_valid());
		CHECK(int(playback->get("stream_error")) == OK);
		playback->set_paused(true);
		OS::get_singleton()->delay_usec(50000);
		double paused_position = playback->get_playback_position();
		for (int i = 0; i < 10; i++) {
			playback->update(0.02);
			OS::get_singleton()->delay_usec(20000);
		}
		CHECK(Math::is_equal_approx(playback->get_playback_position(), paused_position));
		// The initial play completion is deferred and may arrive after readiness
		// becomes visible. Start recording explicit seeks only after playback.
		SIGNAL_DISCARD("seek_completed");
		metadata_events.clear();
		start = OS::get_singleton()->get_ticks_usec();
		playback->seek(5.0);
		playback->seek(6.0);
		CHECK(OS::get_singleton()->get_ticks_usec() - start < 250000);
		CHECK(wait_ready(playback));
		CHECK(Math::is_equal_approx(playback->get_playback_position(), 6.0));
		CHECK(playback->is_paused());
		Dictionary latest_metadata = playback->get_stream_metadata();
		CHECK(String(latest_metadata["title"]) == resolved_title);
		CHECK(stream->get_name() == "Authoritative project title");
		CHECK_FALSE(metadata_events.is_empty());
		for (const Dictionary &event : metadata_events) {
			CHECK(int64_t(event["generation"]) == int64_t(latest_metadata["generation"]));
			CHECK(int64_t(event["playback_instance_id"]) == int64_t(uint64_t(playback->get_instance_id())));
		}
		Array completed;
		Array arguments;
		arguments.append(6.0);
		completed.append(arguments);
		SIGNAL_CHECK("seek_completed", completed);
		SIGNAL_CHECK_FALSE("ready");
		playback->set_paused(false);
		playback->stop();
	}
	SIGNAL_UNWATCH(playback.ptr(), "ready");
	SIGNAL_UNWATCH(playback.ptr(), "seek_completed");
	playback.unref();
	MessageQueue::get_singleton()->flush();
}

TEST_CASE("[SceneTree][NativeStreaming][Network] Paired inputs share playback controls without blocking" * doctest::skip()) {
	check_playback(OS::get_singleton()->get_environment("GODOT_MEDIA_TEST_URL"), true);
}

TEST_CASE("[SceneTree][NativeStreaming][Network][YouTubePlayback] Original YouTube URL resolves and decodes paired media" * doctest::skip()) {
	check_playback(OS::get_singleton()->get_environment("GODOT_YOUTUBE_LIVE_URL"), false);
}

TEST_CASE("[SceneTree][NativeStreaming][Network] Audio resource streams PCM without blocking its mix callback" * doctest::skip()) {
	initialize_audio();
	TestMediaStream::load_certificates();
	String url = OS::get_singleton()->get_environment("GODOT_MEDIA_TEST_URL");
	REQUIRE(!url.is_empty());
	if (url.is_empty()) {
		return;
	}
	Ref<AudioStreamMedia> stream;
	stream.instantiate();
	stream->set_file(url);
	uint64_t start = OS::get_singleton()->get_ticks_usec();
	Ref<AudioStreamPlayback> playback = stream->instantiate_playback();
	CHECK(stream->get_length() == 0);
	CHECK(OS::get_singleton()->get_ticks_usec() - start < 250000);
	REQUIRE(playback.is_valid());
	if (playback.is_null()) {
		return;
	}
	SIGNAL_WATCH(playback.ptr(), "ready");
	SIGNAL_WATCH(playback.ptr(), "seek_completed");
	bool ready = wait_ready(playback);
	CHECK(ready);
	if (ready) {
		Array expected;
		expected.append(Array());
		SIGNAL_CHECK("ready", expected);
		CHECK(stream->get_length() > 10);
		playback->start();
		CHECK(wait_ready(playback));
		SIGNAL_DISCARD("seek_completed");
		AudioFrame samples[1024];
		double energy = 0;
		uint64_t max_mix_usec = 0;
		uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 15000000;
		while (playback->get_playback_position() < 2.0 && playback->is_playing() && OS::get_singleton()->get_ticks_usec() < deadline) {
			start = OS::get_singleton()->get_ticks_usec();
			int count = playback->mix(samples, 1.0, 1024);
			max_mix_usec = MAX(max_mix_usec, OS::get_singleton()->get_ticks_usec() - start);
			for (int i = 0; i < count; i++) {
				energy += Math::abs(samples[i].left) + Math::abs(samples[i].right);
			}
			OS::get_singleton()->delay_usec(20000);
		}
		CHECK(playback->get_playback_position() >= 2.0);
		CHECK(energy > 0);
		CHECK(max_mix_usec < 250000);
		playback->seek(5.0);
		playback->seek(6.0);
		CHECK(wait_ready(playback));
		CHECK(Math::is_equal_approx(playback->get_playback_position(), 6.0));
		Array arguments;
		arguments.append(6.0);
		Array completed;
		completed.append(arguments);
		SIGNAL_CHECK("seek_completed", completed);
		SIGNAL_CHECK_FALSE("ready");
		playback->stop();
		CHECK_FALSE(playback->is_playing());
		CHECK_FALSE(bool(playback->get("stream_ready")));
	}
	SIGNAL_UNWATCH(playback.ptr(), "ready");
	SIGNAL_UNWATCH(playback.ptr(), "seek_completed");
	playback.unref();
	MessageQueue::get_singleton()->flush();
}

TEST_CASE("[SceneTree][NativeStreaming] Rejected destinations emit errors without readiness or seek completion") {
	initialize_audio();
	Ref<VideoStreamMedia> video;
	video.instantiate();
	video->set_file("https://127.0.0.1/media.mp4");
	Ref<AudioStreamMedia> audio;
	audio.instantiate();
	audio->set_file("https://127.0.0.1/media.m4a");
	for (bool is_audio : { false, true }) {
		Ref<RefCounted> playback = is_audio ? Ref<RefCounted>(audio->instantiate_playback()) : Ref<RefCounted>(video->instantiate_playback());
		REQUIRE(playback.is_valid());
		if (playback.is_null()) {
			return;
		}
		SIGNAL_WATCH(playback.ptr(), "ready");
		SIGNAL_WATCH(playback.ptr(), "seek_completed");
		SIGNAL_WATCH(playback.ptr(), "error");
		uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 5000000;
		while (int(playback->get("stream_error")) == OK && OS::get_singleton()->get_ticks_usec() < deadline) {
			MessageQueue::get_singleton()->flush();
			OS::get_singleton()->delay_usec(2000);
		}
		MessageQueue::get_singleton()->flush();
		CHECK(int(playback->get("stream_error")) != OK);
		CHECK_FALSE(bool(playback->get("stream_ready")));
		CHECK_FALSE(bool(playback->get("buffering")));
		SIGNAL_CHECK_FALSE("ready");
		SIGNAL_CHECK_FALSE("seek_completed");
		Array arguments;
		arguments.append(int(ERR_CANT_OPEN));
		arguments.append(is_audio ? "Could not open or seek the audio stream." : "Could not open the video media container (FFmpeg error -13).");
		Array errors;
		errors.append(arguments);
		SIGNAL_CHECK("error", errors);
		SIGNAL_UNWATCH(playback.ptr(), "ready");
		SIGNAL_UNWATCH(playback.ptr(), "seek_completed");
		SIGNAL_UNWATCH(playback.ptr(), "error");
	}
}
TEST_CASE("[SceneTree][NativeStreaming][Network] Queued PCM cannot advance the consumed audio master clock" * doctest::skip()) {
	String url = OS::get_singleton()->get_environment("GODOT_MEDIA_TEST_URL");
	REQUIRE(!url.is_empty());
	TestMediaStream::load_certificates();
	initialize_audio();
	Ref<VideoStreamMedia> stream;
	stream.instantiate();
	stream->set_file(url);
	stream->set_audio_file(url);
	Ref<VideoStreamPlayback> playback = stream->instantiate_playback();
	REQUIRE(playback.is_valid());
	AudioSink sink;
	playback->set_mix_callback(AudioSink::mix, &sink);
	playback->play();
	REQUIRE(wait_ready(playback));
	for (int i = 0; i < 40; i++) {
		playback->set_audio_playback_position(0.0);
		playback->update(0.016);
		MessageQueue::get_singleton()->flush();
		OS::get_singleton()->delay_usec(16000);
	}
	CHECK(sink.frames > playback->get_mix_rate() / 4);
	CHECK(playback->get_playback_position() < 0.1);
	uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 3000000;
	while (playback->get_playback_position() < 0.95 && OS::get_singleton()->get_ticks_usec() < deadline) {
		playback->set_audio_playback_position(1.0);
		playback->update(0.016);
		OS::get_singleton()->delay_usec(16000);
	}
	CHECK(playback->get_playback_position() >= 0.95);
	CHECK(playback->get_playback_position() <= 1.0);
	playback->set_paused(true);
	playback->seek(6.0);
	REQUIRE(wait_ready(playback));
	CHECK(Math::abs(playback->get_playback_position() - 6.0) < 0.1);
	playback->set_paused(false);
	for (int i = 0; i < 20; i++) {
		playback->set_audio_playback_position(6.0);
		playback->update(0.016);
		OS::get_singleton()->delay_usec(16000);
	}
	CHECK(Math::abs(playback->get_playback_position() - 6.0) < 0.1);
	CHECK(int(playback->get("stream_error")) == OK);
	playback->stop();
}

struct LiveVideoWindow {
	double start_position = 0;
	double end_position = 0;
	double decoded_audio_seconds = 0;
	double audio_energy = 0;
	double final_audio_clock = 0;
	double elapsed_wall_seconds = 0;
	double presentation_wait_seconds = 0;
	bool completed_requested_audio = false;
	uint64_t max_update_usec = 0;
	int progressing_windows = 0;
	int sampled_windows = 0;
};

static LiveVideoWindow run_live_video_window(const Ref<VideoStreamPlayback> &p_playback, AudioSink &p_sink, double p_seconds) {
	LiveVideoWindow result;
	result.start_position = p_playback->get_playback_position();
	double last_position = result.start_position;
	const int64_t start_frames = p_sink.frames;
	int64_t last_frames = start_frames;
	const double start_energy = p_sink.energy;
	const int mix_rate = p_playback->get_mix_rate();
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	// Readiness means the inputs/decoders opened, not that decoded media is
	// already buffered. Require the requested amount of consumed PCM, with a
	// bounded allowance for live segment availability and decoder preparation.
	const uint64_t deadline = start + uint64_t((p_seconds + 10.0) * 1000000);
	const int64_t target_frames = int64_t(p_seconds * MAX(mix_rate, 1));
	uint64_t sample_at = start + 4000000;
	uint64_t previous_update = start;
	double fractional_frames = 0;
	while (p_sink.frames - start_frames < target_frames && OS::get_singleton()->get_ticks_usec() < deadline && p_playback->is_playing() && int(p_playback->get("stream_error")) == OK) {
		const uint64_t update_start = OS::get_singleton()->get_ticks_usec();
		const double elapsed_delta = double(update_start - previous_update) / 1000000.0;
		previous_update = update_start;
		fractional_frames += elapsed_delta * mix_rate;
		p_sink.live_available_frames = int64_t(fractional_frames);
		fractional_frames -= p_sink.live_available_frames;
		// Consume only the samples due in this output interval. Unused whole
		// sample capacity represents device underflow, not debt to consume in a
		// later burst; unaccepted PCM stays queued in the playback instance.
		p_playback->set_audio_playback_position(p_sink.clock_origin + (mix_rate > 0 ? double(p_sink.frames) / mix_rate : 0.0));
		// Match a real game frame's delta, including work between updates. A
		// fixed 20ms delta plus a 20ms sleep underdrives playback under CPU load.
		p_playback->update(elapsed_delta);
		// Hardware consumption has now happened. Publish its fresh clock rather
		// than making the decode worker wait for the next game update tick.
		p_playback->set_audio_playback_position(p_sink.clock_origin + (mix_rate > 0 ? double(p_sink.frames) / mix_rate : 0.0));
		result.max_update_usec = MAX(result.max_update_usec, OS::get_singleton()->get_ticks_usec() - update_start);
		MessageQueue::get_singleton()->flush();
		const uint64_t now = OS::get_singleton()->get_ticks_usec();
		if (now >= sample_at) {
			const double position = p_playback->get_playback_position();
			if (position > last_position + 0.1 && p_sink.frames > last_frames + mix_rate / 4) {
				result.progressing_windows++;
			}
			result.sampled_windows++;
			last_position = position;
			last_frames = p_sink.frames;
			sample_at = now + 4000000;
		}
		OS::get_singleton()->delay_usec(20000);
	}
	result.decoded_audio_seconds = mix_rate > 0 ? double(p_sink.frames - start_frames) / mix_rate : 0.0;
	result.final_audio_clock = p_sink.clock_origin + (mix_rate > 0 ? double(p_sink.frames) / mix_rate : 0.0);
	result.end_position = p_playback->get_playback_position();
	// Stop consuming PCM and allow at most 150ms for the worker to present the
	// final consumed clock. Samples taken after that deadline cannot pass the
	// endpoint check; persistent decoder/network lag still fails unchanged.
	p_sink.live_available_frames = 0;
	const uint64_t presentation_start = OS::get_singleton()->get_ticks_usec();
	const uint64_t presentation_deadline = presentation_start + 150000;
	while (OS::get_singleton()->get_ticks_usec() < presentation_deadline) {
		const uint64_t update_start = OS::get_singleton()->get_ticks_usec();
		p_playback->set_audio_playback_position(result.final_audio_clock);
		p_playback->update(0.0);
		result.max_update_usec = MAX(result.max_update_usec, OS::get_singleton()->get_ticks_usec() - update_start);
		MessageQueue::get_singleton()->flush();
		const double position = p_playback->get_playback_position();
		const uint64_t sampled_at = OS::get_singleton()->get_ticks_usec();
		if (sampled_at > presentation_deadline) {
			break;
		}
		result.end_position = position;
		result.presentation_wait_seconds = double(sampled_at - presentation_start) / 1000000.0;
		if (Math::abs(result.final_audio_clock - position) < 0.15) {
			break;
		}
		OS::get_singleton()->delay_usec(MIN(uint64_t(2000), presentation_deadline - sampled_at));
	}
	result.audio_energy = p_sink.energy - start_energy;
	result.elapsed_wall_seconds = double(OS::get_singleton()->get_ticks_usec() - start) / 1000000.0;
	result.completed_requested_audio = p_sink.frames - start_frames >= target_frames;
	return result;
}

static void check_live_video_window(const Ref<VideoStreamPlayback> &p_playback, const LiveVideoWindow &p_window, double p_seconds, const char *p_stage) {
	const double skew = Math::abs(p_window.final_audio_clock - p_window.end_position);
	print_line(vformat("Live playback %s: video %.3f -> %.3f, decoded PCM %.3fs, audio clock %.3f, skew %.3fs, wall %.3fs, presentation sample wait %.3fs, progressing windows %d/%d.", p_stage, p_window.start_position, p_window.end_position, p_window.decoded_audio_seconds, p_window.final_audio_clock, skew, p_window.elapsed_wall_seconds, p_window.presentation_wait_seconds, p_window.progressing_windows, p_window.sampled_windows));
#ifdef DEV_ENABLED
	if (!p_window.completed_requested_audio) {
		p_playback->set(SNAME("_ffmpeg_debug_state_requested"), true);
		const uint64_t snapshot_deadline = OS::get_singleton()->get_ticks_usec() + 100000;
		Dictionary snapshot;
		while (snapshot.is_empty() && OS::get_singleton()->get_ticks_usec() < snapshot_deadline) {
			OS::get_singleton()->delay_usec(2000);
			snapshot = p_playback->get(SNAME("_ffmpeg_debug_state"));
		}
		print_line("Live stalled state: " + JSON::stringify(snapshot));
	}
#endif
	INFO(p_stage);
	CHECK(p_window.completed_requested_audio);
	CHECK(p_window.end_position - p_window.start_position >= MAX(0.5, p_seconds - 2.0));
	CHECK(p_window.decoded_audio_seconds >= MAX(0.5, p_seconds - 2.0));
	CHECK(p_window.audio_energy > 0);
	CHECK(skew < 0.15);
	CHECK(p_window.max_update_usec < 250000);
	CHECK((p_window.sampled_windows > 0 || p_seconds < 4));
	CHECK(p_window.progressing_windows == p_window.sampled_windows);
	CHECK(p_playback->get_texture().is_valid());
	CHECK(p_playback->is_playing());
	CHECK(int(p_playback->get("stream_error")) == OK);
	CHECK(Math::is_finite(p_playback->get_length()));
	CHECK(p_playback->get_length() >= 0);
}

static void report_live_reopen_error(int p_code, const String &p_message) {
	print_line(vformat("Live reopened error: code %d, message: %s", p_code, p_message));
}

static String dvr_fixture(int p_sequence = 100) {
	String text = "#EXTM3U\n#EXT-X-TARGETDURATION:5\n#EXT-X-MEDIA-SEQUENCE:" + itos(p_sequence) + "\n#EXT-X-PROGRAM-DATE-TIME:2026-11-06T12:00:00.125+00:00\n";
	for (int i = 0; i < 20; i++) {
		text += "#EXTINF:5.0,\nhttps://example.com/segment" + itos(i) + ".ts?signature=untouched%2Fvalue\n";
	}
	return text;
}

TEST_CASE("[NativeStreaming][YouTubeDVR] Manifest selection preserves signed segments and bounds the timeline") {
	Ref<FFmpegDVRWindow> window;
	window.instantiate();
	String body;
	double input_start = -1;
	REQUIRE(window->prepare(dvr_fixture(), true, true, body, input_start));
	Dictionary state = window->snapshot();
	CHECK(bool(state["available"]));
	CHECK(Math::abs(double(state["end_utc"]) - double(state["start_utc"]) - 90) < 0.001);
	CHECK(Math::abs(double(state["end_utc"]) - double(state["selected_utc"]) - 15) < 0.001);
	CHECK(Math::abs(double(state["selected_utc"]) - input_start - 5) < 0.001);
	CHECK(body.contains("#EXT-X-MEDIA-SEQUENCE:116\n"));
	CHECK(body.contains("https://example.com/segment16.ts?signature=untouched%2Fvalue"));
	CHECK_FALSE(body.contains("segment15.ts"));
	CHECK_FALSE(body.contains("#EXT-X-ENDLIST"));
	CHECK_FALSE(body.contains("#EXT-X-PROGRAM-DATE-TIME:"));
	// The second input uses the same UTC target, and reloads retain history.
	double audio_start = -1;
	String audio;
	REQUIRE(window->prepare(dvr_fixture(), false, true, audio, audio_start));
	CHECK(audio == body);
	CHECK(audio_start == input_start);
	REQUIRE(window->prepare(dvr_fixture(), true, false, audio, audio_start));
	CHECK(audio == dvr_fixture());
	CHECK(double(window->snapshot()["selected_utc"]) == double(state["selected_utc"]));
	int64_t floor = -1;
	REQUIRE(window->prepare(dvr_fixture(), true, true, body, input_start, &floor));
	CHECK(floor == 116);
	REQUIRE(window->prepare(dvr_fixture(), true, false, audio, audio_start, &floor));
	CHECK(audio == body);
	Ref<FFmpegDVRWindow> from_position;
	from_position.instantiate();
	from_position->requested_position = 25;
	REQUIRE(from_position->prepare(dvr_fixture(), true, true, body, input_start));
	CHECK(Math::abs(double(from_position->snapshot()["selected_utc"]) - double(state["start_utc"]) - 25) < 0.001);
	// Requests outside the advertised window clamp to actually available data.
	for (double requested : { 0.0, 9999999999.0 }) {
		Ref<FFmpegDVRWindow> clamped;
		clamped.instantiate();
		clamped->requested_utc = requested;
		REQUIRE(clamped->prepare(dvr_fixture(), true, true, body, input_start));
		Dictionary bounded = clamped->snapshot();
		CHECK(double(bounded["selected_utc"]) >= double(bounded["start_utc"]));
		CHECK(double(bounded["selected_utc"]) <= double(bounded["live_utc"]));
	}
	for (const String &suffix : { String("#EXT-X-DISCONTINUITY\n"), String("#EXT-X-ENDLIST\n"), String("#EXT-X-KEY:METHOD=AES-128,URI=\"key\"\n"), String("#EXT-X-BYTERANGE:123\n") }) {
		CHECK_FALSE(window->prepare(dvr_fixture() + suffix, true, true, body, input_start));
	}
	CHECK_FALSE(window->prepare(dvr_fixture().replace("5.0,", "nan,"), true, true, body, input_start));
	CHECK_FALSE(window->prepare(dvr_fixture().replace(".125+00:00", ""), true, true, body, input_start));
	CHECK_FALSE(window->prepare(dvr_fixture(INT32_MAX).replace(itos(INT32_MAX), "9223372036854775807"), true, true, body, input_start));
}

TEST_CASE("[SceneTree][YouTubeDVRPlayback][Network] YouTube rewinds paired streams and returns to the live edge" * doctest::skip()) {
	const String url = OS::get_singleton()->get_environment("GODOT_YOUTUBE_DVR_URL");
	REQUIRE(!url.is_empty());
	TestMediaStream::load_certificates();
	initialize_audio();
	Ref<VideoStreamMedia> stream;
	stream.instantiate();
	stream->set_file(url);
	stream->set_live_dvr_enabled(true);
	Ref<VideoStreamPlayback> playback = stream->instantiate_playback();
	REQUIRE(playback.is_valid());
	playback->connect("error", callable_mp_static(report_live_reopen_error));
	playback->play();
	REQUIRE(wait_ready(playback));
	Dictionary window = playback->get_dvr_window();
	print_line("YouTube DVR opened: " + JSON::stringify(window));
	REQUIRE(bool(window.get("available", false)));
	CHECK(double(window["end"]) - double(window["start"]) > 120);
	CHECK(Math::abs(playback->get_playback_position() - double(window["opened_position"])) < 0.1);
	CHECK(double(window["opened_position"]) <= double(window["live_position"]));
	Dictionary metadata = playback->get_stream_metadata();
	CHECK(String(metadata["source"]) == url);
	CHECK(String(metadata["title_source"]) == "youtube");
	CHECK(!String(metadata["title"]).is_empty());
	CHECK(bool(metadata["opened"]));
	AudioSink sink;
	sink.clock_mix_rate = playback->get_mix_rate();
	playback->set_mix_callback(AudioSink::mix, &sink);
	auto run_stage = [&](double seconds, const char *stage) {
		sink.frames = 0;
		sink.energy = 0;
		sink.clock_origin = playback->get_playback_position();
		check_live_video_window(playback, run_live_video_window(playback, sink, seconds), seconds, stage);
	};
	run_stage(4, "DVR initial live");
	const double target = playback->get_playback_position() - 90;
	uint64_t command_start = OS::get_singleton()->get_ticks_usec();
	playback->seek(target);
	CHECK(OS::get_singleton()->get_ticks_usec() - command_start < 250000);
	REQUIRE(wait_ready(playback));
	CHECK(Math::abs(playback->get_playback_position() - target) < 0.1);
	run_stage(8, "DVR rewound");
	playback->set_paused(true);
	const double paused = playback->get_playback_position();
	OS::get_singleton()->delay_usec(250000);
	CHECK(playback->get_playback_position() == paused);
	// An expired/oldest request clamps instead of failing the whole stream.
	playback->seek(0);
	REQUIRE(wait_ready(playback));
	CHECK(playback->is_paused());
	window = playback->get_dvr_window();
	// The advertised start may advance while the worker prebuffers. Samples
	// already fetched at the fixed opening target remain playable afterwards.
	CHECK(Math::abs(playback->get_playback_position() - double(window["opened_position"])) < 0.1);
	CHECK(double(window["opened_position"]) >= 0);
	CHECK(double(window["opened_position"]) <= double(window["start"]));
	playback->set_paused(false);
	run_stage(4, "DVR oldest");
	command_start = OS::get_singleton()->get_ticks_usec();
	REQUIRE(playback->seek_to_live());
	CHECK(OS::get_singleton()->get_ticks_usec() - command_start < 250000);
	REQUIRE(wait_ready(playback));
	window = playback->get_dvr_window();
	CHECK(Math::abs(playback->get_playback_position() - double(window["opened_position"])) < 0.1);
	CHECK(double(window["opened_position"]) <= double(window["live_position"]));
	run_stage(24, "DVR returned live");
	playback->stop();
	playback.unref();
	MessageQueue::get_singleton()->flush();
}

TEST_CASE("[SceneTree][YouTubeDVRAudio][Network] YouTube audio rewinds and returns to live without blocking mixing" * doctest::skip()) {
	const String url = OS::get_singleton()->get_environment("GODOT_YOUTUBE_DVR_URL");
	REQUIRE(!url.is_empty());
	TestMediaStream::load_certificates();
	initialize_audio();
	Ref<AudioStreamMedia> stream;
	stream.instantiate();
	stream->set_file(url);
	stream->set_live_dvr_enabled(true);
	Ref<AudioStreamPlayback> playback = stream->instantiate_playback();
	REQUIRE(playback.is_valid());
	playback->connect("error", callable_mp_static(report_live_reopen_error));
	playback->start();
	REQUIRE(wait_ready(playback));
	Dictionary window = playback->call("get_dvr_window");
	print_line("YouTube DVR audio opened: " + JSON::stringify(window));
	REQUIRE(bool(window.get("available", false)));
	for (int stage = 0; stage < 3; stage++) {
		if (stage == 1) {
			const double target = playback->get_playback_position() - 60;
			playback->seek(target);
			REQUIRE(wait_ready(playback));
			CHECK(Math::abs(playback->get_playback_position() - target) < 0.1);
		} else if (stage == 2) {
			REQUIRE(bool(playback->call("seek_to_live")));
			REQUIRE(wait_ready(playback));
			window = playback->call("get_dvr_window");
			CHECK(Math::abs(playback->get_playback_position() - double(window["opened_position"])) < 0.1);
		}
		AudioFrame samples[1024];
		const double initial = playback->get_playback_position();
		const double seconds = stage == 2 ? 24 : 4;
		const double initial_end = Dictionary(playback->call("get_dvr_window"))["end"];
		double energy = 0;
		double sample_debt = 0;
		uint64_t max_mix = 0;
		const uint64_t began = OS::get_singleton()->get_ticks_usec();
		uint64_t previous_mix = began;
		const uint64_t deadline = began + uint64_t((seconds + 8) * 1000000);
		while (playback->get_playback_position() - initial < seconds && OS::get_singleton()->get_ticks_usec() < deadline && int(playback->get("stream_error")) == OK) {
			const uint64_t now = OS::get_singleton()->get_ticks_usec();
			sample_debt += double(now - previous_mix) * AudioServer::get_singleton()->get_mix_rate() / 1000000.0;
			previous_mix = now;
			// Match elapsed hardware demand; Windows may round a fixed sleep
			// up to a scheduler tick and otherwise underdrive this endurance test.
			for (int batch = 0; batch < 8 && sample_debt >= 1024; batch++) {
				const uint64_t start = OS::get_singleton()->get_ticks_usec();
				const int count = playback->mix(samples, 1, 1024);
				max_mix = MAX(max_mix, OS::get_singleton()->get_ticks_usec() - start);
				sample_debt -= 1024;
				for (int i = 0; i < count; i++) {
					energy += Math::abs(samples[i].left) + Math::abs(samples[i].right);
				}
			}
			MessageQueue::get_singleton()->flush();
			OS::get_singleton()->delay_usec(2000);
		}
		print_line(vformat("YouTube DVR audio stage %d: %.3f -> %.3f, energy %.3f, max mix %dus, wall %.3fs.", stage, initial, playback->get_playback_position(), energy, max_mix, double(OS::get_singleton()->get_ticks_usec() - began) / 1000000));
		CHECK(playback->get_playback_position() - initial >= seconds);
		CHECK(stream->get_length() >= playback->get_playback_position());
		if (stage == 2) {
			CHECK(playback->get_playback_position() > initial_end + 3);
			CHECK(stream->get_length() > initial_end);
		}
		CHECK(energy > 0);
		CHECK(max_mix < 250000);
		CHECK(int(playback->get("stream_error")) == OK);
	}
	playback->stop();
	playback.unref();
	MessageQueue::get_singleton()->flush();
}

TEST_CASE("[SceneTree][LiveStreaming][Network] Live paired media advances across segments and supports pause cancellation and reopening" * doctest::skip()) {
	const String url = OS::get_singleton()->get_environment("GODOT_MEDIA_LIVE_TEST_URL");
	REQUIRE(!url.is_empty());
	TestMediaStream::load_certificates();
	initialize_audio();
	Ref<VideoStreamMedia> stream;
	stream.instantiate();
	stream->set_file(url);
	const String audio_url = OS::get_singleton()->get_environment("GODOT_MEDIA_LIVE_TEST_AUDIO_URL");
	if (!audio_url.is_empty()) {
		stream->set_audio_file(audio_url);
	}
	uint64_t start = OS::get_singleton()->get_ticks_usec();
	Ref<VideoStreamPlayback> playback = stream->instantiate_playback();
	CHECK(OS::get_singleton()->get_ticks_usec() - start < 250000);
	REQUIRE(playback.is_valid());
	AudioSink sink;
	playback->set_mix_callback(AudioSink::mix, &sink);
	SIGNAL_WATCH(playback.ptr(), "ready");
	SIGNAL_WATCH(playback.ptr(), "error");
	playback->play();
	const bool ready = wait_ready(playback);
	CHECK(ready);
	INFO("Live opening error: " << int(playback->get("stream_error")));
	if (ready) {
		// The caller selects a genuinely live manifest without ENDLIST. The
		// 24-second window crosses at least three segments for <=7s samples.
		check_live_video_window(playback, run_live_video_window(playback, sink, 24), 24, "sustained");
		Array expected_ready;
		expected_ready.append(Array());
		SIGNAL_CHECK("ready", expected_ready);
		playback->set_paused(true);
		OS::get_singleton()->delay_usec(50000);
		const double paused_position = playback->get_playback_position();
		const int64_t paused_frames = sink.frames;
		for (int i = 0; i < 20; i++) {
			playback->update(0.05);
			MessageQueue::get_singleton()->flush();
			OS::get_singleton()->delay_usec(50000);
		}
		CHECK(Math::is_equal_approx(playback->get_playback_position(), paused_position));
		CHECK(sink.frames == paused_frames);
		playback->set_paused(false);
		check_live_video_window(playback, run_live_video_window(playback, sink, 5), 5, "resumed");
		SIGNAL_CHECK_FALSE("ready");
		SIGNAL_CHECK_FALSE("error");
	}
	start = OS::get_singleton()->get_ticks_usec();
	playback->stop();
	CHECK(OS::get_singleton()->get_ticks_usec() - start < 250000);
	CHECK_FALSE(bool(playback->get("stream_ready")));
	CHECK_FALSE(playback->is_playing());
	SIGNAL_UNWATCH(playback.ptr(), "ready");
	SIGNAL_UNWATCH(playback.ptr(), "error");
	playback.unref();
	MessageQueue::get_singleton()->flush();

	// Cancel a fresh opening and keep the object alive long enough to detect
	// stale notifications. A subsequent fresh instance must still decode.
	playback = stream->instantiate_playback();
	REQUIRE(playback.is_valid());
	SIGNAL_WATCH(playback.ptr(), "ready");
	SIGNAL_WATCH(playback.ptr(), "seek_completed");
	SIGNAL_WATCH(playback.ptr(), "error");
	playback->play();
	OS::get_singleton()->delay_usec(10000);
	start = OS::get_singleton()->get_ticks_usec();
	playback->stop();
	CHECK(OS::get_singleton()->get_ticks_usec() - start < 250000);
	for (int i = 0; i < 10; i++) {
		MessageQueue::get_singleton()->flush();
		OS::get_singleton()->delay_usec(10000);
	}
	CHECK_FALSE(bool(playback->get("stream_ready")));
	SIGNAL_CHECK_FALSE("ready");
	SIGNAL_CHECK_FALSE("seek_completed");
	SIGNAL_CHECK_FALSE("error");
	SIGNAL_UNWATCH(playback.ptr(), "ready");
	SIGNAL_UNWATCH(playback.ptr(), "seek_completed");
	SIGNAL_UNWATCH(playback.ptr(), "error");
	start = OS::get_singleton()->get_ticks_usec();
	playback.unref();
	CHECK(OS::get_singleton()->get_ticks_usec() - start < 2000000);
	MessageQueue::get_singleton()->flush();

	playback = stream->instantiate_playback();
	REQUIRE(playback.is_valid());
	AudioSink reopened_sink;
	playback->set_mix_callback(AudioSink::mix, &reopened_sink);
	const Callable reopen_error = callable_mp_static(report_live_reopen_error);
	playback->connect(SNAME("error"), reopen_error);
	playback->play();
	const bool reopened = wait_ready(playback);
	CHECK(reopened);
	if (reopened) {
		// A fresh injected decoder failure can occur again after reopening.
		// Observe enough media to pass the next live keyframe and prove recovery.
		const double reopen_seconds = OS::get_singleton()->get_environment("GODOT_FFMPEG_TEST_HW_FAILURE") == "1" ? 12.0 : 3.0;
		check_live_video_window(playback, run_live_video_window(playback, reopened_sink, reopen_seconds), reopen_seconds, "reopened");
	} else {
		// Readiness/error properties can change just before their deferred
		// notification is queued. Keep the error observer alive to log its
		// sanitized stage/context, rather than retaining only ERR_CANT_OPEN.
		for (int i = 0; i < 10; i++) {
			OS::get_singleton()->delay_usec(2000);
			MessageQueue::get_singleton()->flush();
		}
	}
	playback->disconnect(SNAME("error"), reopen_error);
	playback->stop();
	playback.unref();
	MessageQueue::get_singleton()->flush();
}

TEST_CASE("[SceneTree][LiveStreaming][Network] Audio resource decodes a live input without blocking its mix callback" * doctest::skip()) {
	String url = OS::get_singleton()->get_environment("GODOT_MEDIA_LIVE_TEST_AUDIO_URL");
	if (url.is_empty()) {
		url = OS::get_singleton()->get_environment("GODOT_MEDIA_LIVE_TEST_URL");
	}
	REQUIRE(!url.is_empty());
	TestMediaStream::load_certificates();
	initialize_audio();
	Ref<AudioStreamMedia> stream;
	stream.instantiate();
	stream->set_file(url);
	uint64_t start = OS::get_singleton()->get_ticks_usec();
	Ref<AudioStreamPlayback> playback = stream->instantiate_playback();
	CHECK(OS::get_singleton()->get_ticks_usec() - start < 250000);
	CHECK(stream->get_length() == 0);
	REQUIRE(playback.is_valid());
	playback->start();
	const bool ready = wait_ready(playback);
	CHECK(ready);
	if (ready) {
		CHECK(Math::is_finite(stream->get_length()));
		CHECK(stream->get_length() >= 0);
		AudioFrame samples[1024];
		double energy = 0;
		int64_t frames = 0;
		uint64_t max_mix_usec = 0;
		const double initial_position = playback->get_playback_position();
		uint64_t previous_mix = OS::get_singleton()->get_ticks_usec();
		const uint64_t deadline = previous_mix + 10000000;
		const double mix_rate = AudioServer::get_singleton()->get_mix_rate();
		double sample_debt = 0;
		while (OS::get_singleton()->get_ticks_usec() < deadline && playback->is_playing() && int(playback->get("stream_error")) == OK) {
			const uint64_t now = OS::get_singleton()->get_ticks_usec();
			sample_debt += double(now - previous_mix) * mix_rate / 1000000.0;
			previous_mix = now;
			// Simulate a hardware callback's elapsed sample demand. A fixed one
			// block per sleep underdrives audio when other work delays this test.
			for (int batch = 0; batch < 8 && sample_debt >= 1024; batch++) {
				start = OS::get_singleton()->get_ticks_usec();
				const int count = playback->mix(samples, 1.0, 1024);
				max_mix_usec = MAX(max_mix_usec, OS::get_singleton()->get_ticks_usec() - start);
				sample_debt -= 1024;
				frames += count;
				for (int i = 0; i < count; i++) {
					energy += Math::abs(samples[i].left) + Math::abs(samples[i].right);
				}
			}
			MessageQueue::get_singleton()->flush();
			OS::get_singleton()->delay_usec(20000);
		}
		const double played_seconds = playback->get_playback_position() - initial_position;
		print_line(vformat("Live audio: position advanced %.3fs, mixed frames %d, energy %.3f, maximum mix time %dus.", played_seconds, frames, energy, max_mix_usec));
		CHECK(played_seconds >= 8);
		CHECK(frames > 0);
		CHECK(energy > 0);
		CHECK(max_mix_usec < 250000);
		CHECK(playback->is_playing());
		CHECK(int(playback->get("stream_error")) == OK);
	}
	start = OS::get_singleton()->get_ticks_usec();
	playback->stop();
	CHECK(OS::get_singleton()->get_ticks_usec() - start < 250000);
	CHECK_FALSE(playback->is_playing());
	CHECK_FALSE(bool(playback->get("stream_ready")));
	playback.unref();
	MessageQueue::get_singleton()->flush();
}

#ifdef DEV_ENABLED
TEST_CASE("[SceneTree][HLSEpoch][Local] Declared dated fMP4 epochs play through the captured reset" * doctest::skip()) {
	const String path = OS::get_singleton()->get_environment("GODOT_MEDIA_EPOCH_TEST_FILE");
	REQUIRE(!path.is_empty());
	REQUIRE(!FFmpegFileIO::is_remote(path));
	REQUIRE(FileAccess::exists(path));
	REQUIRE(FileAccess::get_file_as_string(path).contains("#EXT-X-ENDLIST"));
	REQUIRE(OS::get_singleton()->get_environment("GODOT_FFMPEG_FORCE_SOFTWARE") == "1");
	initialize_audio();
	SafeFlag cancelled;
	String open_error;
	Ref<VideoStreamPlayback> playback = ffmpeg_open_video_on_worker(path, Dictionary(), &cancelled, String(), Dictionary(), &open_error);
	INFO(open_error.utf8().get_data());
	REQUIRE(playback.is_valid());
	const Dictionary opened = playback->get(SNAME("_ffmpeg_debug_state"));
	REQUIRE(double(opened.get("origin", 0.0)) > 946684800.0);
	playback->play();
	const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 10000000;
	double virtual_seconds = 0.0;
	while (playback->is_playing() && int(playback->get(SNAME("stream_error"))) == OK && OS::get_singleton()->get_ticks_usec() < deadline) {
		const double delta = virtual_seconds < 25.0 ? 0.025 : 0.0;
		playback->update(delta);
		virtual_seconds += delta;
	}
	const Dictionary finished = playback->get(SNAME("_ffmpeg_debug_state"));
	print_line("Declared HLS epoch: " + JSON::stringify(finished));
	CHECK(int(playback->get(SNAME("stream_error"))) == OK);
	CHECK(int64_t(finished.get("negative_frames", 0)) == 0);
	CHECK(double(finished.get("last_decoded_position", 0.0)) > 19.0);
	CHECK(int64_t(finished.get("decoded_frames", 0)) > 400);
	CHECK_FALSE(playback->is_playing());
	playback.unref();
}

TEST_CASE("[SceneTree][HLSEpoch][Local] Dated HLS audio reconciles the captured encoder overlap" * doctest::skip()) {
	const String path = OS::get_singleton()->get_environment("GODOT_MEDIA_EPOCH_TEST_AUDIO_FILE");
	REQUIRE(FileAccess::exists(path));
	initialize_audio();
	SafeFlag cancelled;
	Ref<AudioStreamPlayback> playback = ffmpeg_audio_playback_from_buffer(Vector<uint8_t>(), nullptr, path, Ref<AudioStream>(), Dictionary(), &cancelled);
	REQUIRE(playback.is_valid());
	Dictionary state = playback->get_parameter(SNAME("_ffmpeg_hls_timeline"));
	REQUIRE(bool(state.get("enabled", false)));
	playback->start();
	AudioFrame samples[1024];
	int64_t frames = 0;
	double energy = 0.0;
	while (playback->is_playing() && frames < 30 * AudioServer::get_singleton()->get_mix_rate()) {
		const int mixed = playback->mix(samples, 1.0, 1024);
		frames += mixed;
		for (int i = 0; i < mixed; i++) {
			energy += Math::abs(samples[i].left) + Math::abs(samples[i].right);
		}
	}
	state = playback->get_parameter(SNAME("_ffmpeg_hls_timeline"));
	state["seconds"] = double(frames) / AudioServer::get_singleton()->get_mix_rate();
	print_line("Declared HLS audio: " + JSON::stringify(state));
	CHECK(int(playback->get_parameter(SNAME("stream_error"))) == OK);
	CHECK(energy > 1.0);
	CHECK(double(state["seconds"]) > 19.9);
	CHECK(double(state["seconds"]) < 20.2);
	CHECK(int64_t(state.get("trimmed_frames", 0)) > 1000);
	CHECK_FALSE(playback->is_playing());
}

TEST_CASE("[SceneTree][HLSEpoch][Local] Paired dated epochs keep the consumed audio clock" * doctest::skip()) {
	const String path = OS::get_singleton()->get_environment("GODOT_MEDIA_EPOCH_TEST_FILE");
	const String audio_path = OS::get_singleton()->get_environment("GODOT_MEDIA_EPOCH_TEST_AUDIO_FILE");
	REQUIRE(FileAccess::exists(path));
	REQUIRE(FileAccess::exists(audio_path));
	initialize_audio();
	for (bool queued : { false, true }) {
		SafeFlag cancelled;
		String error;
		Ref<VideoStreamPlayback> playback = ffmpeg_open_video_on_worker(path, Dictionary(), &cancelled, audio_path, Dictionary(), &error);
		INFO(error.utf8().get_data());
		REQUIRE(playback.is_valid());
		AudioSink sink;
		sink.clock_mix_rate = playback->get_mix_rate();
		playback->set_mix_callback(AudioSink::mix, &sink);
		playback->set_audio_playback_position_callback(AudioSink::read_consumed_clock, &sink);
		playback->play();
		if (queued) {
			REQUIRE(ffmpeg_start_video_read_ahead(playback, &error));
		}
		const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 15000000;
		double maximum_skew = 0.0;
		double virtual_time = 0.0;
		while (playback->is_playing() && int(playback->get(SNAME("stream_error"))) == OK && OS::get_singleton()->get_ticks_usec() < deadline) {
			const double before = playback->get_playback_position();
			// Advance the sample clock only after video has caught up. Network
			// producer scheduling must not masquerade as sustained decode loss.
			const double audio_clock = AudioSink::read_consumed_clock(&sink);
			const double delta = audio_clock - before < 0.06 ? 0.02 : 0.0;
			playback->update(delta);
			virtual_time += delta;
			if (playback->get_playback_position() > before) {
				maximum_skew = MAX(maximum_skew, Math::abs(playback->get_playback_position() - AudioSink::read_consumed_clock(&sink)));
			}
			if (virtual_time > 26.0) {
				break;
			}
		}
		const Dictionary state = playback->get(SNAME("_ffmpeg_debug_state"));
		print_line(vformat("Paired HLS epoch: queued %s, PCM %.6fs, video %.6fs, maximum observed skew %.6fs, error %d.", queued, AudioSink::read_consumed_clock(&sink), playback->get_playback_position(), maximum_skew, int(playback->get(SNAME("stream_error")))));
		CHECK(int(playback->get(SNAME("stream_error"))) == OK);
		CHECK(AudioSink::read_consumed_clock(&sink) > 19.9);
		CHECK(playback->get_playback_position() > 19.0);
		CHECK(sink.energy > 1.0);
		CHECK(maximum_skew < 0.15);
		CHECK(int64_t(state.get("negative_frames", 0)) == 0);
		CHECK_FALSE(playback->is_playing());
		playback.unref();
	}
}

TEST_CASE("[SceneTree][HLSEpoch][Local] Dated epochs can seek forward and backward across the boundary" * doctest::skip()) {
	const String path = OS::get_singleton()->get_environment("GODOT_MEDIA_EPOCH_TEST_FILE");
	const String audio_path = OS::get_singleton()->get_environment("GODOT_MEDIA_EPOCH_TEST_AUDIO_FILE");
	initialize_audio();
	SafeFlag cancelled;
	Ref<VideoStreamPlayback> playback = ffmpeg_open_video_on_worker(path, Dictionary(), &cancelled);
	REQUIRE(playback.is_valid());
	playback->play();
	for (double target : { 14.0, 4.0, 10.0 }) {
		playback->seek(target);
		const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 3000000;
		while (playback->get_playback_position() < target + 0.5 && playback->is_playing() && int(playback->get(SNAME("stream_error"))) == OK && OS::get_singleton()->get_ticks_usec() < deadline) {
			playback->update(0.025);
		}
		print_line(vformat("HLS epoch seek %.3f reached %.6f, error %d.", target, playback->get_playback_position(), int(playback->get(SNAME("stream_error")))));
		CHECK(int(playback->get(SNAME("stream_error"))) == OK);
		CHECK(playback->get_playback_position() >= target + 0.5);
		CHECK(playback->get_playback_position() < target + 1.0);
	}
	Ref<AudioStreamPlayback> audio = ffmpeg_audio_playback_from_buffer(Vector<uint8_t>(), nullptr, audio_path, Ref<AudioStream>(), Dictionary(), &cancelled);
	REQUIRE(audio.is_valid());
	AudioFrame samples[1024];
	audio->start();
	for (double target : { 14.0, 4.0, 10.0 }) {
		audio->seek(target);
		CHECK(audio->mix(samples, 1.0, 1024) == 1024);
		CHECK(int(audio->get_parameter(SNAME("stream_error"))) == OK);
		CHECK(Math::abs(audio->get_playback_position() - target - 1024.0 / AudioServer::get_singleton()->get_mix_rate()) < 0.0001);
	}
}

TEST_CASE("[SceneTree][HLSEpoch][Local] Unannounced or undated resets still stop with an explicit error" * doctest::skip()) {
	const String path = OS::get_singleton()->get_environment("GODOT_MEDIA_EPOCH_TEST_FILE");
	REQUIRE(FileAccess::exists(path));
	const String original = FileAccess::get_file_as_string(path);
	initialize_audio();
	for (int variant = 0; variant < 4; variant++) {
		const String fixture = OS::get_singleton()->get_cache_path().path_join(vformat("godot-hls-invalid-%d.m3u8", variant));
		Ref<FileAccess> file = FileAccess::open(fixture, FileAccess::WRITE);
		REQUIRE(file.is_valid());
		String body;
		bool after_boundary = false;
		for (const String &raw_line : original.split("\n")) {
			String line = raw_line.strip_edges();
			after_boundary = after_boundary || line == "#EXT-X-DISCONTINUITY";
			if ((variant == 0 && line == "#EXT-X-DISCONTINUITY") ||
					(variant == 1 && line.begins_with("#EXT-X-PROGRAM-DATE-TIME:")) ||
					(variant == 2 && after_boundary && line.begins_with("#EXT-X-PROGRAM-DATE-TIME:"))) {
				continue;
			}
			if (variant == 3 && after_boundary && line.begins_with("#EXT-X-PROGRAM-DATE-TIME:")) {
				line = "#EXT-X-PROGRAM-DATE-TIME:2026-11-08T11:23:17+01:00";
			}
			body += line + "\n";
		}
		file->store_string(body);
		file.unref();
		SafeFlag cancelled;
		Ref<VideoStreamPlayback> playback = ffmpeg_open_video_on_worker(fixture, Dictionary(), &cancelled);
		REQUIRE(playback.is_valid());
		playback->play();
		const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 3000000;
		while (playback->is_playing() && int(playback->get(SNAME("stream_error"))) == OK && OS::get_singleton()->get_ticks_usec() < deadline) {
			playback->update(0.05);
		}
		CHECK(int(playback->get(SNAME("stream_error"))) == ERR_UNAVAILABLE);
		CHECK_FALSE(playback->is_playing());
		CHECK_FALSE(String(playback->get(SNAME("_ffmpeg_error_message"))).is_empty());
		playback.unref();
		DirAccess::remove_absolute(fixture);
	}
}

TEST_CASE("[SceneTree][HLSEpoch][Local] Declared audio gaps pad the clock without unbounded buffering" * doctest::skip()) {
	const String path = OS::get_singleton()->get_environment("GODOT_MEDIA_EPOCH_TEST_AUDIO_FILE");
	REQUIRE(FileAccess::exists(path));
	const String fixture = OS::get_singleton()->get_cache_path().path_join("godot-hls-audio-gap.m3u8");
	Ref<FileAccess> file = FileAccess::open(fixture, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	file->store_string(FileAccess::get_file_as_string(path).replace("11:23:17+01:00", "11:23:19+01:00"));
	file.unref();
	initialize_audio();
	SafeFlag cancelled;
	Ref<AudioStreamPlayback> audio = ffmpeg_audio_playback_from_buffer(Vector<uint8_t>(), nullptr, fixture, Ref<AudioStream>(), Dictionary(), &cancelled);
	REQUIRE(audio.is_valid());
	audio->set_parameter(SNAME("_ffmpeg_start_read_ahead"), true);
	audio->start();
	AudioFrame samples[1024];
	int64_t frames = 0;
	const int rate = AudioServer::get_singleton()->get_mix_rate();
	const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 10000000;
	while (audio->is_playing() && frames < rate * 25 && OS::get_singleton()->get_ticks_usec() < deadline) {
		const int count = audio->mix(samples, 1.0, 1024);
		frames += count;
		if (!count) {
			OS::get_singleton()->delay_usec(1000);
		}
	}
	const Dictionary timeline = audio->get_parameter(SNAME("_ffmpeg_hls_timeline"));
	print_line(vformat("HLS audio gap: %.6fs, state %s.", double(frames) / rate, JSON::stringify(timeline)));
	CHECK(int(audio->get_parameter(SNAME("stream_error"))) == OK);
	CHECK(double(frames) / rate > 21.9);
	CHECK(double(frames) / rate < 22.2);
	CHECK(int64_t(timeline.get("padded_frames", 0)) > rate);
	CHECK(int64_t(timeline.get("padded_frames", 0)) < 2 * rate);
	CHECK_FALSE(audio->is_playing());
	audio.unref();
	DirAccess::remove_absolute(fixture);
}

TEST_CASE("[SceneTree][HLSEpoch][Local] Finite playlist PCM retains decoder samples across the same reset" * doctest::skip()) {
	const String path = OS::get_singleton()->get_environment("GODOT_MEDIA_EPOCH_TEST_AUDIO_FILE");
	REQUIRE(FileAccess::exists(path));
	initialize_audio();
	SafeFlag cancelled;
	Dictionary result;
	const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 10000000;
	REQUIRE(ffmpeg_decode_audio_pcm(path, Dictionary(), cancelled, 44100, 4 * 1024 * 1024, deadline, result) == OK);
	// Independently decoded SDK reference: 899 AAC frames, 1024 samples each.
	// The real-time path trims 39392 overlapping samples in this fixture;
	// bounded offline preparation must retain all actual decoder samples.
	CHECK(int64_t(result.get("frame_count", 0)) == 899 * 1024);
	CHECK(int64_t(result.get("pcm_bytes", 0)) == 899 * 1024 * 4);
	CHECK(int(result.get("mix_rate", 0)) == 44100);
	CHECK(Math::abs(double(result.get("duration", 0.0)) - 899.0 * 1024.0 / 44100.0) < 0.000001);
	CHECK(result.get("stream", Variant()).get_type() == Variant::OBJECT);
}

TEST_CASE("[SceneTree][LiveOrigin][Network] Delayed live input opening prepares both tracks at a common source PTS" * doctest::skip()) {
	const String url = OS::get_singleton()->get_environment("GODOT_MEDIA_LIVE_TEST_URL");
	const String audio_url = OS::get_singleton()->get_environment("GODOT_MEDIA_LIVE_TEST_AUDIO_URL");
	const int delay_ms = OS::get_singleton()->get_environment("GODOT_FFMPEG_TEST_LIVE_AUDIO_DELAY_MSEC").to_int();
	REQUIRE(!url.is_empty());
	// Use a direct HLS fixture with <=7s segments. A delay longer than one
	// segment deliberately puts the second input on a later playlist edge.
	REQUIRE(delay_ms >= 8000);
	REQUIRE(delay_ms <= 10000);
	TestMediaStream::load_certificates();
	initialize_audio();
	SafeFlag cancelled;
	Ref<VideoStreamPlayback> playback = ffmpeg_open_video_on_worker(url, Dictionary(), &cancelled, audio_url, Dictionary());
	REQUIRE(playback.is_valid());
	const Dictionary origins = playback->get("_ffmpeg_live_origin");
	REQUIRE(bool(origins.get("live_hls", false)));
	REQUIRE(bool(origins.get("audio_live_hls", false)));
	REQUIRE(origins.get("video_source_origin", Variant()).get_type() != Variant::NIL);
	REQUIRE(origins.get("audio_source_origin", Variant()).get_type() != Variant::NIL);
	REQUIRE(origins.get("audio_clock_origin", Variant()).get_type() != Variant::NIL);
	const double original_video_origin = origins["video_source_origin"];
	const double audio_origin = origins["audio_source_origin"];
	const double video_clock_origin = origins["video_clock_origin"];
	const double audio_clock_origin = origins["audio_clock_origin"];
	print_line(vformat("Live origin regression: video input %.6f, delayed audio input %.6f, video clock %.6f, audio clock %.6f, input separation %.6fs.", original_video_origin, audio_origin, video_clock_origin, audio_clock_origin, Math::abs(original_video_origin - audio_origin)));
	CHECK(Math::is_finite(original_video_origin));
	CHECK(Math::is_finite(audio_origin));
	CHECK(Math::abs(original_video_origin - audio_origin) >= 1.0);
	CHECK(Math::is_equal_approx(video_clock_origin, MAX(original_video_origin, audio_origin)));
	CHECK(Math::is_equal_approx(video_clock_origin, audio_clock_origin));
	AudioSink sink;
	sink.clock_mix_rate = playback->get_mix_rate();
	playback->set_mix_callback(AudioSink::mix, &sink);
	// This direct internal decoder can consume PCM and decode video in the
	// same update. Read its freshly consumed clock during that decode, as the
	// asynchronous wrapper does, rather than holding the pre-update value.
	playback->set_audio_playback_position_callback(AudioSink::read_consumed_clock, &sink);
	playback->play();
	String read_ahead_error;
	const bool read_ahead_ready = ffmpeg_start_video_read_ahead(playback, &read_ahead_error);
	INFO("Live origin packet prefill: ", read_ahead_error.utf8().get_data());
	REQUIRE(read_ahead_ready);
	// Transfer both demuxers to packet readers only after common-origin
	// preparation, matching the asynchronous player's ownership handoff.
	const LiveVideoWindow decoded = run_live_video_window(playback, sink, 3);
	const double presented_source_pts = video_clock_origin + decoded.end_position;
	const double consumed_audio_source_pts = audio_clock_origin + decoded.final_audio_clock;
	print_line(vformat("Live origin regression decoded video source %.6f, audio source %.6f, PCM %.3fs, video position %.3f.", presented_source_pts, consumed_audio_source_pts, decoded.decoded_audio_seconds, decoded.end_position));
	CHECK(decoded.completed_requested_audio);
	CHECK(decoded.end_position >= 1.5);
	CHECK(decoded.decoded_audio_seconds >= 1.5);
	CHECK(decoded.audio_energy > 0);
	CHECK(Math::abs(presented_source_pts - consumed_audio_source_pts) < 0.15);
	CHECK(int(playback->get("stream_error")) == OK);
	const uint64_t close_started = OS::get_singleton()->get_ticks_usec();
	playback.unref();
	CHECK(OS::get_singleton()->get_ticks_usec() - close_started < 250000);
	cancelled.set();
	MessageQueue::get_singleton()->flush();
}
#endif
} // namespace TestNativeStreaming
#endif
