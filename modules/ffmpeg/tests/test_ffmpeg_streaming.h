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

#include "modules/ffmpeg/ffmpeg_adaptive_quality.h"
#include "modules/ffmpeg/ffmpeg_async_playback.h"
#include "modules/ffmpeg/ffmpeg_file_io.h"
#include "modules/ffmpeg/ffmpeg_frame_queue.h"
#include "modules/ffmpeg/ffmpeg_media_metadata.h"
#include "modules/ffmpeg/ffmpeg_packet_reader.h"
#include "modules/ffmpeg/video_stream_media.h"
#include "modules/media_stream/media_http.h"
#include "modules/media_stream/tests/media_stream_test_utils.h"

#include <cstring>

Ref<AudioStreamPlayback> ffmpeg_test_audio_packet_reader(const Vector<uint8_t> &p_wave, FFmpegPacketReader::TestReadCallback p_read, void *p_userdata);

namespace TestNativeStreaming {

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

struct FiniteVideoFixture {
	SafeNumeric<int> owners{ 0 };
	SafeFlag produced;
	bool live = true;
	Error terminal_error = OK;
	int mix_rate = 0;
	bool empty = false;
	bool cpu_frames = false;
	bool prepared_cpu_frame = false;
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
			r_value = int64_t(fixture->prepared_cpu_frame || emitted > 0 ? 1 : 0);
		} else if (p_name == SNAME("_ffmpeg_cpu_frame")) {
			r_value = fixture->prepared_cpu_frame || emitted > 0 ? fixture->image : Ref<Image>();
		} else if (p_name == SNAME("_ffmpeg_audio_active")) {
			r_value = playing;
		} else if (p_name == SNAME("_ffmpeg_live_hls")) {
			r_value = fixture->live;
		} else if (p_name == SNAME("_ffmpeg_packet_waiting")) {
			r_value = false;
		} else if (p_name == SNAME("_ffmpeg_presentable_position")) {
			r_value = 0.2;
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
	double get_playback_position() const override { return fixture->prepared_cpu_frame || emitted > 0 ? 0.2 : 0.0; }
	Ref<Texture2D> get_texture() const override { return Ref<Texture2D>(); }
	int get_channels() const override { return 2; }
	int get_mix_rate() const override { return fixture->mix_rate; }
	void update(double) override {
		if (!playing || paused || !mix_callback) {
			return;
		}
		float samples[2048];
		for (float &sample : samples) {
			sample = 0.25f;
		}
		while (!fixture->empty && emitted < fixture->mix_rate) {
			const int count = MIN(1024, fixture->mix_rate - emitted);
			const int accepted = mix_callback(mix_udata, samples, count);
			emitted += accepted;
			if (accepted < count) {
				return;
			}
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

static Vector<uint8_t> packet_test_wave(int p_rate) {
	Vector<uint8_t> wave;
	wave.resize(44 + 2 * 1024 * 4);
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
TEST_CASE("[SceneTree][HLSEpoch][Local] An unsupported HLS timestamp reset reports an error and stops" * doctest::skip()) {
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
	print_line("Local HLS epoch opening: " + open_error);
	REQUIRE(playback.is_valid());
	const Dictionary opened = playback->get(SNAME("_ffmpeg_debug_state"));
	// The trusted local fixture begins with independent old-period frames,
	// then a new MAP whose source timestamps retreat by hundreds of seconds.
	// A probe starting in the new period would not exercise this regression.
	REQUIRE(double(opened.get("origin", 0.0)) > 100.0);
	playback->play();
	const uint64_t started = OS::get_singleton()->get_ticks_usec();
	const uint64_t deadline = started + 5000000;
	double virtual_seconds = 0.0;
	while (playback->is_playing() && int(playback->get(SNAME("stream_error"))) == OK && OS::get_singleton()->get_ticks_usec() < deadline) {
		// There is no audio track in this finite fixture. Drive the video-only
		// clock directly rather than pretending an audio callback consumed PCM.
		const double delta = virtual_seconds < 25.0 ? 0.05 : 0.0;
		playback->update(delta);
		virtual_seconds += delta;
		// Publishing a usable frame can yield before the next packet read.
		// Once the target clock reaches 25s, retire remaining frames at that
		// fixed clock until the bounded fixture ends or reports the reset.
	}
	const Dictionary finished = playback->get(SNAME("_ffmpeg_debug_state"));
	const bool decoded_reset = int64_t(finished.get("negative_frames", 0)) > 0 || double(finished.get("last_decoded_position", 0.0)) < 0.0;
	print_line(vformat("Local HLS epoch: virtual %.3fs, wall %.3fs, error %d, decoded reset %s, state %s.", virtual_seconds, double(OS::get_singleton()->get_ticks_usec() - started) / 1000000.0, int(playback->get(SNAME("stream_error"))), decoded_reset ? "yes" : "no", JSON::stringify(finished)));
	// Finite EOF alone is insufficient: silently discarding a whole new epoch
	// would otherwise look like normal completion while live inputs hang.
	CHECK(int(playback->get(SNAME("stream_error"))) == ERR_UNAVAILABLE);
	const String error_message = playback->get(SNAME("_ffmpeg_error_message"));
	print_line("Local HLS epoch error: " + error_message);
	CHECK((error_message.contains("timestamps") && error_message.contains("discontinuity")));
	CHECK_FALSE(playback->is_playing());
	playback.unref();
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
