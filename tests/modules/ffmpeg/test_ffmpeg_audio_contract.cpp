/**************************************************************************/
/*  test_ffmpeg_audio_contract.cpp                                      */
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

TEST_FORCE_LINK(test_ffmpeg_audio_contract)

#include "modules/modules_enabled.gen.h"
#if defined(MODULE_FFMPEG_ENABLED) && defined(DEV_ENABLED)
#include "core/io/file_access.h"
#include "core/io/marshalls.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "scene/resources/audio_stream_polyphonic.h"
#include "servers/audio/audio_driver_dummy.h"
#include "servers/audio/audio_server.h"
#include "tests/test_utils.h"

#include "modules/ffmpeg/video_stream_media.h"
#include "modules/mp3/audio_stream_mp3.h"
#include "modules/vorbis/audio_stream_ogg_vorbis.h"

#include <cstring>

namespace TestFFmpegAudioContract {
struct MixSettings {
	int previous_rate = AudioDriverDummy::get_dummy_singleton()->get_mix_rate();
	float previous_speed = AudioServer::get_singleton()->get_playback_speed_scale();
	MixSettings(int p_rate = 44100) {
		AudioDriverDummy::get_dummy_singleton()->set_mix_rate(p_rate);
		AudioServer::get_singleton()->set_playback_speed_scale(1.0);
	}
	~MixSettings() {
		AudioDriverDummy::get_dummy_singleton()->set_mix_rate(previous_rate);
		AudioServer::get_singleton()->set_playback_speed_scale(previous_speed);
	}
};

static float signal(int p_index) {
	return float((p_index % 257 - 128) * 128) / 32768.0f;
}
static Vector<uint8_t> wave(int p_frames, int p_rate = 44100) {
	Vector<uint8_t> bytes;
	if (bytes.resize(44 + p_frames * 4) != OK) {
		return bytes;
	}
	uint8_t *data = bytes.ptrw();
	memcpy(data, "RIFF", 4);
	encode_uint32(bytes.size() - 8, data + 4);
	memcpy(data + 8, "WAVEfmt ", 8);
	encode_uint32(16, data + 16);
	encode_uint16(1, data + 20);
	encode_uint16(2, data + 22);
	encode_uint32(p_rate, data + 24);
	encode_uint32(p_rate * 4, data + 28);
	encode_uint16(4, data + 32);
	encode_uint16(16, data + 34);
	memcpy(data + 36, "data", 4);
	encode_uint32(p_frames * 4, data + 40);
	for (int i = 0; i < p_frames; i++) {
		const int16_t value = int16_t((i % 257 - 128) * 128);
		encode_uint16(uint16_t(value), data + 44 + i * 4);
		encode_uint16(uint16_t(-value), data + 46 + i * 4);
	}
	return bytes;
}
static FFmpegAudioWorkStats stats(const Ref<AudioStreamPlayback> &p_playback) {
	FFmpegAudioWorkStats result;
	CHECK(ffmpeg_audio_get_work_stats(p_playback, result));
	CHECK(result.work_used >= 0);
	CHECK(result.work_used <= result.work_limit);
	CHECK(result.decoder_opens == 1);
	return result;
}
static Ref<AudioStreamPlayback> from_path(const String &p_path, bool p_video = false, double p_origin = 0.0) {
	return ffmpeg_audio_playback_from_buffer(Vector<uint8_t>(), nullptr, TestUtils::get_data_path("media/" + p_path), Ref<AudioStream>(), p_video, p_origin);
}
static int first_nonzero(const Vector<AudioFrame> &p_frames, int p_count) {
	for (int i = 0; i < p_count; i++) {
		if (Math::abs(p_frames[i].left) > 0.00001 || Math::abs(p_frames[i].right) > 0.00001) {
			return i;
		}
	}
	return -1;
}

TEST_CASE("[Audio][F13][FFmpegAudio] Exact unity-rate PCM tails survive short streams and decoder boundaries") {
	MixSettings settings;
	for (int length : { 17, 127, 128, 129, 4097 }) {
		Ref<AudioStreamPlayback> playback = ffmpeg_audio_playback_from_buffer(wave(length));
		REQUIRE(playback.is_valid());
		if (playback.is_null()) {
			return;
		}
		Vector<AudioFrame> frames;
		REQUIRE(frames.resize(length + 256) == OK);
		if (frames.is_empty()) {
			return;
		}
		playback->start_playback();
		const int mixed = playback->mix(frames.ptrw(), 1.0, frames.size());
		CHECK(mixed == length);
		CHECK_FALSE(playback->is_playing());
		for (int i = 0; i < length; i++) {
			CHECK(frames[i].left == doctest::Approx(signal(i)).epsilon(0.00001));
			CHECK(frames[i].right == doctest::Approx(-signal(i)).epsilon(0.00001));
		}
		for (int i = length; i < frames.size(); i++) {
			CHECK(frames[i].left == 0.0f);
			CHECK(frames[i].right == 0.0f);
		}
		stats(playback);
	}
}

TEST_CASE("[Audio][F13][FFmpegAudio] Pitch and global speed consume the expected interpolated PCM positions") {
	MixSettings settings;
	struct Rate {
		float pitch;
		float speed;
	};
	for (const Rate rate : { Rate{ 0.5, 1.0 }, Rate{ 1.0, 1.0 }, Rate{ 2.0, 1.0 }, Rate{ 1.0, 2.0 }, Rate{ 2.0, 0.5 }, Rate{ 0.5, 0.5 } }) {
		AudioServer::get_singleton()->set_playback_speed_scale(rate.speed);
		const int length = 4097;
		const double step = rate.pitch * rate.speed;
		const int expected = int(Math::ceil(length / step));
		Ref<AudioStreamPlayback> playback = ffmpeg_audio_playback_from_buffer(wave(length));
		REQUIRE(playback.is_valid());
		if (playback.is_null()) {
			return;
		}
		Vector<AudioFrame> frames;
		REQUIRE(frames.resize(expected + 64) == OK);
		if (frames.is_empty()) {
			return;
		}
		playback->start_playback();
		CHECK(playback->mix(frames.ptrw(), rate.pitch, frames.size()) == expected);
		CHECK_FALSE(playback->is_playing());
		for (int i = 0; i < expected; i++) {
			const double position = i * step;
			const int index = int(position);
			const float a = signal(index);
			const float b = index + 1 < length ? signal(index + 1) : 0.0f;
			const float value = a + (b - a) * float(position - index);
			CHECK(frames[i].left == doctest::Approx(value).epsilon(0.00002));
			CHECK(frames[i].right == doctest::Approx(-value).epsilon(0.00002));
		}
		stats(playback);
	}
}

TEST_CASE("[Audio][F13][FFmpegAudio] Live pitch and global-rate changes retain the source cursor") {
	MixSettings settings;
	Ref<AudioStreamPlayback> playback = ffmpeg_audio_playback_from_buffer(wave(4097));
	REQUIRE(playback.is_valid());
	if (playback.is_null()) {
		return;
	}
	AudioFrame frames[256];
	playback->start_playback();
	CHECK(playback->mix(frames, 1.0, 256) == 256);
	CHECK(playback->mix(frames, 2.0, 256) == 256);
	AudioServer::get_singleton()->set_playback_speed_scale(0.5);
	CHECK(playback->mix(frames, 1.0, 256) == 256);
	CHECK(playback->get_playback_position() == doctest::Approx(896.0 / 44100).epsilon(0.000001));
	playback->stop_playback();
	CHECK_FALSE(playback->is_playing());
}

TEST_CASE("[Audio][F13][FFmpegAudio] SWR buffering and final drain retain resampled short and full-second streams") {
	MixSettings settings(48000);
	for (int source_rate : { 8000, 44100, 96000, 192000 }) {
		for (int length : { 17, source_rate }) {
			Ref<AudioStreamPlayback> playback = ffmpeg_audio_playback_from_buffer(wave(length, source_rate));
			REQUIRE(playback.is_valid());
			if (playback.is_null()) {
				return;
			}
			playback->start_playback();
			int total = 0;
			float peak = 0.0f;
			AudioFrame frames[513];
			for (int i = 0; i < 200 && playback->is_playing(); i++) {
				const int count = playback->mix(frames, 1.0, 513);
				CHECK(count >= 0);
				CHECK(count <= 513);
				if (count < 0 || count > 513) {
					return;
				}
				total += count;
				for (int j = 0; j < count; j++) {
					peak = MAX(peak, Math::abs(frames[j].left));
				}
				stats(playback);
			}
			CHECK_FALSE(playback->is_playing());
			const int expected = int((int64_t(length) * 48000 + source_rate - 1) / source_rate);
			CHECK(total == expected);
			CHECK(peak > 0.1f);
		}
	}
}

TEST_CASE("[Audio][F12][FFmpegAudio] Timestamped containers use indexed preroll and exact bounded position correction") {
	MixSettings settings;
	Ref<AudioStreamPlayback> playback = ffmpeg_audio_playback_from_buffer(wave(5 * 44100));
	REQUIRE(playback.is_valid());
	if (playback.is_null()) {
		return;
	}
	playback->start_playback(4.0);
	AudioFrame frames[128];
	bool reached = false;
	int batches = 0;
	uint64_t maximum_usec = 0;
	for (; batches < 16 && !reached; batches++) {
		const uint64_t begin = OS::get_singleton()->get_ticks_usec();
		CHECK(playback->mix(frames, 1.0, 128) == 128);
		maximum_usec = MAX(maximum_usec, OS::get_singleton()->get_ticks_usec() - begin);
		stats(playback);
		reached = playback->get_playback_position() > 4.0;
	}
	REQUIRE(reached);
	if (!reached) {
		return;
	}
	CHECK(stats(playback).indexed_seeks == 1);
	CHECK(batches <= 6);
	for (int i = 0; i < 128; i++) {
		CHECK(frames[i].left == doctest::Approx(signal(4 * 44100 + i)).epsilon(0.00001));
		CHECK(frames[i].right == doctest::Approx(-signal(4 * 44100 + i)).epsilon(0.00001));
	}
	print_line(vformat("F12 indexed seek: batches=%d max_callback_usec=%d", batches, maximum_usec));
}

TEST_CASE("[Audio][F12][FFmpegAudio] Raw AAC late seek yields nonterminal blocks and reaches exact sequential PCM") {
	MixSettings settings;
	const Vector<uint8_t> data = FileAccess::get_file_as_bytes(TestUtils::get_data_path("media/ffmpeg_seek_44100.aac"));
	REQUIRE_FALSE(data.is_empty());
	if (data.is_empty()) {
		return;
	}
	Ref<AudioStreamPlayback> reference = ffmpeg_audio_playback_from_buffer(data, "aac");
	Ref<AudioStreamPlayback> playback = ffmpeg_audio_playback_from_buffer(data, "aac");
	REQUIRE(reference.is_valid());
	if (reference.is_null()) {
		return;
	}
	REQUIRE(playback.is_valid());
	if (playback.is_null()) {
		return;
	}
	reference->set_is_sample(true); // Explicit offline reference rendering.
	reference->start_playback();
	AudioFrame discard[4096];
	int remaining = 20 * 44100;
	while (remaining > 0) {
		const int amount = MIN(remaining, 4096);
		REQUIRE(reference->mix(discard, 1.0, amount) == amount);
		remaining -= amount;
	}
	AudioFrame expected[128];
	REQUIRE(reference->mix(expected, 1.0, 128) == 128);
	playback->start_playback(20.0);
	bool yielded = false;
	bool reached = false;
	uint64_t maximum_usec = 0;
	int batches = 0;
	AudioFrame actual[128];
	for (int batch = 0; batch < 1000 && !reached; batch++) {
		const uint64_t begin = OS::get_singleton()->get_ticks_usec();
		CHECK(playback->mix(actual, 1.0, 128) == 128);
		maximum_usec = MAX(maximum_usec, OS::get_singleton()->get_ticks_usec() - begin);
		batches = batch + 1;
		CHECK(playback->is_playing());
		const FFmpegAudioWorkStats work = stats(playback);
		if (playback->get_playback_position() > 20.0) {
			reached = true;
		} else {
			yielded = true;
			CHECK(work.work_used == work.work_limit);
			for (const AudioFrame &sample : actual) {
				CHECK(sample.left == 0);
				CHECK(sample.right == 0);
			}
		}
	}
	REQUIRE(reached);
	if (!reached) {
		return;
	}
	CHECK(yielded);
	print_line(vformat("F12 raw AAC seek: batches=%d max_callback_usec=%d", batches, maximum_usec));
	for (int i = 0; i < 128; i++) {
		CHECK(actual[i].left == doctest::Approx(expected[i].left).epsilon(0.0001));
		CHECK(actual[i].right == doctest::Approx(expected[i].right).epsilon(0.0001));
	}
}

TEST_CASE("[Audio][F12][F13][FFmpegAudio] Dense unrelated video packets yield live work and preserve offline whole-stream rendering") {
	MixSettings settings;
	Ref<AudioStreamPlayback> offline = from_path("ffmpeg_dense_video_pcm.mkv");
	Ref<AudioStreamPlayback> live = from_path("ffmpeg_dense_video_pcm.mkv");
	REQUIRE(offline.is_valid());
	if (offline.is_null()) {
		return;
	}
	REQUIRE(live.is_valid());
	if (live.is_null()) {
		return;
	}
	offline->set_is_sample(true);
	offline->start_playback();
	Vector<AudioFrame> complete;
	REQUIRE(complete.resize(5000) == OK);
	if (complete.is_empty()) {
		return;
	}
	CHECK(offline->mix(complete.ptrw(), 1.0, complete.size()) == 4410);
	CHECK_FALSE(offline->is_playing());
	CHECK(first_nonzero(complete, 4410) >= 0);
	CHECK(stats(offline).work_limit == INT64_MAX);
	live->start_playback();
	bool yielded = false;
	AudioFrame frames[128];
	for (int batch = 0; batch < 1000 && live->is_playing(); batch++) {
		const double before = live->get_playback_position();
		const int mixed = live->mix(frames, 1.0, 128);
		const FFmpegAudioWorkStats work = stats(live);
		if (work.work_used == work.work_limit) {
			yielded = true;
			CHECK(mixed == 128);
			CHECK(live->is_playing());
			CHECK(live->get_playback_position() >= before);
		}
	}
	CHECK(yielded);
	CHECK_FALSE(live->is_playing());
	CHECK(live->get_playback_position() == doctest::Approx(0.1).epsilon(0.00001));
}

TEST_CASE("[Audio][F13][FFmpegAudio] Typed MP3 and Vorbis retain sample ownership and offline decoding") {
	MixSettings settings;
	Ref<AudioStreamMP3> mp3 = AudioStreamMP3::load_from_file(TestUtils::get_data_path("media/native_tone_44100.mp3"));
	Ref<AudioStreamOggVorbis> ogg = AudioStreamOggVorbis::load_from_file(TestUtils::get_data_path("media/ffmpeg_tone_44100.ogg"));
	for (const Ref<AudioStream> &stream : { Ref<AudioStream>(mp3), Ref<AudioStream>(ogg) }) {
		REQUIRE(stream.is_valid());
		if (stream.is_null()) {
			return;
		}
		CHECK(stream->can_be_sampled());
		Ref<AudioStreamPlayback> playback = stream->instantiate_playback();
		REQUIRE(playback.is_valid());
		if (playback.is_null()) {
			return;
		}
		Ref<AudioSamplePlayback> sample;
		sample.instantiate();
		playback->set_is_sample(true);
		playback->set_sample_playback(sample);
		CHECK(playback->get_is_sample());
		CHECK(playback->get_sample_playback() == sample);
		CHECK(sample->stream_playback == playback);
		playback->set_sample_playback(nullptr);
		sample->stream_playback.unref();
		CHECK(playback->get_sample_playback().is_null());
		playback->start_playback();
		Vector<AudioFrame> frames;
		REQUIRE(frames.resize(88200) == OK);
		if (frames.is_empty()) {
			return;
		}
		const int count = playback->mix(frames.ptrw(), 1.0, frames.size());
		const int expected = int(Math::round(stream->get_length() * 44100));
		CHECK(count == expected);
		CHECK_FALSE(playback->is_playing());
		CHECK(first_nonzero(frames, count) >= 0);
		CHECK(stats(playback).work_limit == INT64_MAX);
	}
}

TEST_CASE("[Audio][F12][F13][FFmpegAudio] Live loop metadata nullable overrides and tags remain effective") {
	MixSettings settings;
	Ref<AudioStreamMP3> stream = AudioStreamMP3::load_from_file(TestUtils::get_data_path("media/native_tone_44100.mp3"));
	REQUIRE(stream.is_valid());
	if (stream.is_null()) {
		return;
	}
	stream->set_loop(true);
	stream->set_loop_offset(0.2);
	stream->set_bpm(120);
	stream->set_beat_count(1);
	Ref<AudioStreamPlayback> playback = stream->instantiate_playback();
	REQUIRE(playback.is_valid());
	if (playback.is_null()) {
		return;
	}
	playback->start_playback();
	AudioFrame frames[256];
	for (int i = 0; i < 1000 && playback->get_loop_count() < 2; i++) {
		CHECK(playback->mix(frames, 1.0, 256) == 256);
		stats(playback);
	}
	CHECK(playback->get_loop_count() >= 2);
	CHECK(playback->is_playing());
	CHECK(playback->get_playback_position() >= 0.2);
	CHECK(playback->get_playback_position() < 0.51);
	playback->tag_used_streams();
	CHECK(stream->get_tagged_frame_count() > 0);
	playback->set_parameter("looping", false);
	CHECK(playback->get_parameter("looping") == Variant(false));
	for (int i = 0; i < 1000 && playback->is_playing(); i++) {
		playback->mix(frames, 1.0, 256);
		stats(playback);
	}
	CHECK_FALSE(playback->is_playing());
	playback->set_parameter("looping", Variant());
	CHECK(playback->get_parameter("looping") == Variant());
	stream->set_loop(false);
	playback->start_playback();
	for (int i = 0; i < 1000 && playback->is_playing(); i++) {
		playback->mix(frames, 1.0, 256);
	}
	CHECK_FALSE(playback->is_playing());
	CHECK(playback->get_loop_count() == 0);
}

TEST_CASE("[Audio][F13][FFmpegAudio] Embedded video audio applies its already-scaled clock once") {
	MixSettings settings;
	AudioServer::get_singleton()->set_playback_speed_scale(2.0);
	Ref<AudioStreamPlayback> playback = ffmpeg_audio_playback_from_buffer(wave(4097), nullptr, String(), Ref<AudioStream>(), true, 0.0);
	REQUIRE(playback.is_valid());
	if (playback.is_null()) {
		return;
	}
	playback->start_playback();
	AudioFrame frames[256];
	CHECK(playback->mix(frames, 1.0, 256) == 256);
	CHECK(playback->get_playback_position() == doctest::Approx(256.0 / 44100).epsilon(0.000001));
	for (int i = 0; i < 256; i++) {
		CHECK(frames[i].left == doctest::Approx(signal(i)).epsilon(0.00001));
	}
}

struct SampleDriver : AudioDriver {
	AudioDriver *previous = AudioDriver::get_singleton();
	Mutex mutex;
	Ref<AudioSamplePlayback> current;
	int starts = 0;
	int stops = 0;
	SampleDriver() { set_singleton(); }
	~SampleDriver() {
		if (current.is_valid()) {
			Ref<AudioSamplePlayback> pending = current;
			AudioServer::get_singleton()->stop_sample_playback(pending);
		}
		previous->set_singleton();
	}
	const char *get_name() const override { return "FFmpeg sample contract test"; }
	Error init() override { return OK; }
	void start() override {}
	int get_mix_rate() const override { return 44100; }
	SpeakerMode get_speaker_mode() const override { return SPEAKER_MODE_STEREO; }
	void lock() override { mutex.lock(); }
	void unlock() override { mutex.unlock(); }
	void finish() override {}
	bool is_stream_registered_as_sample(const Ref<AudioStream> &) const override { return true; }
	void start_sample_playback(const Ref<AudioSamplePlayback> &p_sample) override {
		CHECK(p_sample.is_valid());
		CHECK(p_sample->stream_playback.is_valid());
		CHECK(p_sample->stream_playback->get_sample_playback() == p_sample);
		current = p_sample;
		starts++;
	}
	void stop_sample_playback(const Ref<AudioSamplePlayback> &p_sample) override {
		CHECK(p_sample.is_valid());
		CHECK(p_sample->stream_playback.is_valid());
		current.unref();
		stops++;
	}
};

TEST_CASE("[Audio][F13][FFmpegAudio] Polyphonic sample stop and completion retain the required backend reference") {
	MixSettings settings;
	Ref<AudioStreamMP3> mp3 = AudioStreamMP3::load_from_file(TestUtils::get_data_path("media/native_tone_44100.mp3"));
	REQUIRE(mp3.is_valid());
	if (mp3.is_null()) {
		return;
	}
	SampleDriver driver;
	Ref<AudioStreamPolyphonic> stream;
	stream.instantiate();
	stream->set_polyphony(1);
	Ref<AudioStreamPlaybackPolyphonic> playback = stream->instantiate_playback();
	REQUIRE(playback.is_valid());
	if (playback.is_null()) {
		return;
	}
	playback->start_playback();
	const int64_t first = playback->play_stream(mp3, 0, 0, 1, AudioServer::PLAYBACK_TYPE_SAMPLE);
	CHECK(first != AudioStreamPlaybackPolyphonic::INVALID_ID);
	REQUIRE(driver.current.is_valid());
	if (driver.current.is_null()) {
		return;
	}
	Ref<AudioSamplePlayback> finished = driver.current;
	Ref<AudioStreamPlayback> backend = finished->stream_playback;
	CHECK(backend->get_is_sample());
	// The Web driver's completion callback uses this same public AudioServer path.
	AudioServer::get_singleton()->stop_sample_playback(finished);
	CHECK(backend->get_sample_playback().is_null());
	CHECK(driver.stops == 1);
	const int64_t second = playback->play_stream(mp3, 0, 0, 1, AudioServer::PLAYBACK_TYPE_SAMPLE);
	CHECK(second != AudioStreamPlaybackPolyphonic::INVALID_ID);
	CHECK(second != first);
	CHECK_FALSE(playback->is_stream_playing(first));
	playback->stop_stream(second);
	AudioFrame frames[64];
	CHECK(playback->mix(frames, 1.0, 64) == 64);
	CHECK(driver.stops == 2);
	CHECK(driver.current.is_null());
	CHECK_FALSE(playback->is_stream_playing(second));
	playback->stop_playback();
}

TEST_CASE("[Audio][F18][FFmpegAudio] Embedded audio shares the container origin and retains intentional offsets across seek") {
	MixSettings settings;
	struct Input {
		const char *name;
		double origin;
	};
	Vector<AudioFrame> baseline;
	int first = -1;
	int baseline_count = 0;
	for (const Input input : { Input{ "ffmpeg_origin0.mkv", 0.0 }, Input{ "ffmpeg_origin2.mkv", 2.0 }, Input{ "ffmpeg_audio_delayed_origin2.mkv", 2.0 }, Input{ "ffmpeg_video_delayed_origin2.mkv", 2.0 } }) {
		INFO(input.name);
		Ref<AudioStreamPlayback> playback = from_path(input.name, true, input.origin);
		REQUIRE(playback.is_valid());
		if (playback.is_null()) {
			return;
		}
		playback->set_is_sample(true);
		playback->start_playback();
		Vector<AudioFrame> frames;
		REQUIRE(frames.resize(44100) == OK);
		if (frames.is_empty()) {
			return;
		}
		const int count = playback->mix(frames.ptrw(), 1.0, frames.size());
		CHECK(count > 7000);
		CHECK(count < 14000);
		const int begin = first_nonzero(frames, count);
		REQUIRE(begin >= 0);
		if (begin < 0) {
			return;
		}
		if (first < 0) {
			first = begin;
			baseline = frames;
			baseline_count = count;
		}
		if (String(input.name).contains("audio_delayed")) {
			CHECK(begin - first == doctest::Approx(0.075 * 44100).epsilon(0.001));
		} else {
			CHECK(Math::abs(begin - first) <= 1);
			CHECK(count == baseline_count);
			for (int i = 0; i < MIN(count, baseline_count); i++) {
				CHECK(frames[i].left == doctest::Approx(baseline[i].left).epsilon(0.0001));
				CHECK(frames[i].right == doctest::Approx(baseline[i].right).epsilon(0.0001));
			}
		}
		playback->set_is_sample(false);
		playback->start_playback(0.13);
		AudioFrame after_seek[128];
		CHECK(playback->mix(after_seek, 1.0, 128) == 128);
		const int offset = int(0.13 * 44100);
		for (int i = 0; i < 128; i++) {
			CHECK(after_seek[i].left == doctest::Approx(frames[offset + i].left).epsilon(0.0001));
			CHECK(after_seek[i].right == doctest::Approx(frames[offset + i].right).epsilon(0.0001));
		}
		stats(playback);
	}
}

static int accept_audio(void *, const float *, int p_frames) {
	return p_frames;
}

TEST_CASE("[Audio][F18][FFmpegVideo] Shared video origin and intentional video delay survive pause and relative seek") {
	MixSettings settings;
	double positions[3] = {};
	int index = 0;
	for (const char *name : { "ffmpeg_origin0.mkv", "ffmpeg_origin2.mkv", "ffmpeg_video_delayed_origin2.mkv" }) {
		Ref<VideoStreamMedia> stream;
		stream.instantiate();
		stream->set_file(TestUtils::get_data_path(String("media/") + name));
		Ref<VideoStreamPlayback> playback = stream->instantiate_playback();
		REQUIRE(playback.is_valid());
		if (playback.is_null()) {
			return;
		}
		playback->set_mix_callback(accept_audio, nullptr);
		playback->play();
		for (int i = 0; i < 12; i++) {
			playback->update(1.0 / 240);
		}
		positions[index++] = playback->get_playback_position();
		playback->set_paused(true);
		const double paused = playback->get_playback_position();
		playback->update(0.05);
		CHECK(playback->get_playback_position() == paused);
		playback->seek(0.13);
		playback->set_paused(false);
		for (int i = 0; i < 12; i++) {
			playback->update(1.0 / 240);
		}
		CHECK(playback->get_playback_position() >= 0.07);
		CHECK(playback->get_playback_position() < 0.3);
		playback->stop();
	}
	CHECK(positions[0] > 0.02);
	CHECK(positions[1] == doctest::Approx(positions[0]).epsilon(0.001));
	CHECK(positions[2] == 0.0);
}

TEST_CASE("[Audio][F12][FFmpegAudio] Default MP3 generic indexing yields within the shared callback budget") {
	MixSettings settings;
	const String path = TestUtils::get_data_path("media/ffmpeg_seek_generic_44100.mp3");
	const Vector<uint8_t> data = FileAccess::get_file_as_bytes(path);
	REQUIRE_FALSE(data.is_empty());
	if (data.is_empty()) {
		return;
	}
	Ref<AudioStreamPlayback> reference = ffmpeg_audio_playback_from_buffer(data);
	REQUIRE(reference.is_valid());
	if (reference.is_null()) {
		return;
	}
	reference->set_is_sample(true);
	reference->start_playback();
	AudioFrame discard[4096];
	for (int remaining = 20 * 44100; remaining > 0;) {
		const int count = MIN(remaining, 4096);
		REQUIRE(reference->mix(discard, 1.0, count) == count);
		remaining -= count;
	}
	AudioFrame expected[128];
	REQUIRE(reference->mix(expected, 1.0, 128) == 128);
	for (bool file_backed : { false, true }) {
		INFO(file_backed);
		Ref<AudioStreamPlayback> playback = file_backed ? from_path("ffmpeg_seek_generic_44100.mp3") : ffmpeg_audio_playback_from_buffer(data);
		REQUIRE(playback.is_valid());
		if (playback.is_null()) {
			return;
		}
		playback->start_playback(20.0);
		const double start = playback->get_playback_position();
		bool reached = false;
		int yields = 0;
		uint64_t maximum_usec = 0;
		AudioFrame actual[128];
		for (int batch = 0; batch < 1000 && !reached; batch++) {
			const uint64_t begin = OS::get_singleton()->get_ticks_usec();
			CHECK(playback->mix(actual, 1.0, 128) == 128);
			maximum_usec = MAX(maximum_usec, OS::get_singleton()->get_ticks_usec() - begin);
			const FFmpegAudioWorkStats work = stats(playback);
			CHECK(work.seek_io_bytes >= 0);
			CHECK(work.seek_io_bytes <= 32768);
			CHECK(playback->is_playing());
			reached = playback->get_playback_position() > start;
			if (!reached) {
				yields++;
			}
		}
		REQUIRE(reached);
		if (!reached) {
			return;
		}
		CHECK(yields > 0);
		CHECK(stats(playback).indexed_seeks == 1);
		for (int i = 0; i < 128; i++) {
			CHECK(actual[i].left == doctest::Approx(expected[i].left).epsilon(0.0001));
			CHECK(actual[i].right == doctest::Approx(expected[i].right).epsilon(0.0001));
		}
		print_line(vformat("F12 default MP3: file=%d yields=%d max_callback_usec=%d", int(file_backed), yields, maximum_usec));
	}
}

TEST_CASE("[Audio][F12][F13][FFmpegAudio] Resampled seeks preserve the sequential filter phase") {
	MixSettings settings(48000);
	for (int rate : { 8000, 44100, 96000, 192000 }) {
		INFO(rate);
		const Vector<uint8_t> data = wave(5 * rate, rate);
		Ref<AudioStreamPlayback> reference = ffmpeg_audio_playback_from_buffer(data);
		Ref<AudioStreamPlayback> playback = ffmpeg_audio_playback_from_buffer(data);
		REQUIRE(reference.is_valid());
		REQUIRE(playback.is_valid());
		if (reference.is_null() || playback.is_null()) {
			return;
		}
		reference->set_is_sample(true);
		reference->start_playback();
		const double time = 4.12345;
		AudioFrame discard[4096];
		for (int remaining = int(time * 48000); remaining > 0;) {
			const int amount = MIN(remaining, 4096);
			REQUIRE(reference->mix(discard, 1.0, amount) == amount);
			remaining -= amount;
		}
		AudioFrame expected[128], actual[128];
		REQUIRE(reference->mix(expected, 1.0, 128) == 128);
		playback->start_playback(time);
		const double start = playback->get_playback_position();
		bool reached = false;
		for (int batch = 0; batch < 1000 && !reached; batch++) {
			CHECK(playback->mix(actual, 1.0, 128) == 128);
			stats(playback);
			reached = playback->get_playback_position() > start;
		}
		REQUIRE(reached);
		if (!reached) {
			return;
		}
		for (int i = 0; i < 128; i++) {
			CHECK(actual[i].left == doctest::Approx(expected[i].left).epsilon(0.0001));
			CHECK(actual[i].right == doctest::Approx(expected[i].right).epsilon(0.0001));
		}
	}
}

struct AcceptedAudio {
	int frames = 0;
	int calls = 0;
	static int accept(void *p_self, const float *, int p_frames) {
		AcceptedAudio *self = static_cast<AcceptedAudio *>(p_self);
		self->frames += p_frames;
		self->calls++;
		return p_frames;
	}
};

TEST_CASE("[Audio][F12][F18][FFmpegVideo] Embedded audio work yields do not publish fictitious timeline samples") {
	MixSettings settings;
	Ref<VideoStreamMedia> stream;
	stream.instantiate();
	stream->set_file(TestUtils::get_data_path("media/ffmpeg_dense_video_pcm.mkv"));
	Ref<VideoStreamPlayback> playback = stream->instantiate_playback();
	REQUIRE(playback.is_valid());
	if (playback.is_null()) {
		return;
	}
	AcceptedAudio accepted;
	playback->set_mix_callback(AcceptedAudio::accept, &accepted);
	playback->play();
	for (int i = 0; i < 200 && accepted.frames < 4410 && playback->is_playing(); i++) {
		playback->update(128.0 / 44100);
	}
	CHECK(accepted.frames == 4410);
	CHECK(accepted.calls > 1);
	playback->stop();
}

} // namespace TestFFmpegAudioContract
#endif
