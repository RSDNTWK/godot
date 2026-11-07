/**************************************************************************/
/*  test_audio_stream_pcm_decoder.h                                              */
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
#include "core/io/marshalls.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "modules/ffmpeg/audio_stream_pcm_decoder.h"
#include "scene/resources/audio_stream_wav.h"
#include "servers/audio/audio_driver_dummy.h"
#include "servers/audio/audio_server.h"
#include "tests/test_macros.h"
#include "tests/test_utils.h"
#include <cstring>

namespace TestPCMDecode {

static void initialize_audio() {
	if (!AudioServer::get_singleton()) {
		AudioDriverManager::initialize(AudioDriverManager::get_driver_count() - 1);
		AudioServer *server = memnew(AudioServer);
		server->init();
	}
}

static String wave_file(int p_frames, int p_rate) {
	Vector<uint8_t> bytes;
	bytes.resize(44 + p_frames * 4);
	uint8_t *write = bytes.ptrw();
	memcpy(write, "RIFF", 4);
	encode_uint32(bytes.size() - 8, write + 4);
	memcpy(write + 8, "WAVEfmt ", 8);
	encode_uint32(16, write + 16);
	encode_uint16(1, write + 20);
	encode_uint16(2, write + 22);
	encode_uint32(p_rate, write + 24);
	encode_uint32(p_rate * 4, write + 28);
	encode_uint16(4, write + 32);
	encode_uint16(16, write + 34);
	memcpy(write + 36, "data", 4);
	encode_uint32(p_frames * 4, write + 40);
	for (int i = 0; i < p_frames; i++) {
		encode_uint16(1000, write + 44 + i * 4);
		encode_uint16(uint16_t(int16_t(-1000)), write + 46 + i * 4);
	}
	String path = TestUtils::get_temp_path(vformat("pcm-prepare-%d-%d.wav", p_rate, p_frames));
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	file->store_buffer(bytes);
	return path;
}

struct Decode {
	String path;
	SafeFlag cancelled;
	int rate = 48000;
	int64_t limit = 128 * 1024 * 1024;
	uint64_t deadline = 0;
	Dictionary result;
	Error error = FAILED;
	bool off_main = false;
	static void run(void *p_self) {
		Decode *self = static_cast<Decode *>(p_self);
		self->off_main = !Thread::is_main_thread();
		self->error = ffmpeg_decode_audio_pcm(self->path, Dictionary(), self->cancelled, self->rate, self->limit, self->deadline, self->result);
	}
	void execute() {
		Thread thread;
		thread.start(run, this);
		REQUIRE(thread.is_started());
		thread.wait_to_finish();
		CHECK(off_main);
	}
};

TEST_CASE("[PCMDecode] Exact decoded tail and fractional-second loop positions use the output sample grid") {
	initialize_audio();
	const int frames = 34 * 48000 + 13;
	Decode job;
	job.path = wave_file(frames, 48000);
	job.limit = int64_t(frames) * 4;
	job.deadline = OS::get_singleton()->get_ticks_usec() + 10000000;
	job.execute();
	REQUIRE(job.error == OK);
	CHECK(int64_t(job.result["frame_count"]) == frames);
	CHECK(int64_t(job.result["pcm_bytes"]) == frames * 4);
	CHECK(bool(job.result["worker_thread"]));
	Ref<AudioStreamWAV> wav = job.result["stream"];
	REQUIRE(wav.is_valid());
	CHECK(wav->get_mix_rate() == 48000);
	CHECK(wav->get_format() == AudioStreamWAV::FORMAT_16_BITS);
	CHECK(wav->is_stereo());
	const Vector<uint8_t> pcm = wav->get_data();
	CHECK(pcm.size() == frames * 4);
	for (int i : { 0, 4095, 4096, frames - 1 }) {
		CHECK(int16_t(decode_uint16(pcm.ptr() + i * 4)) == 1000);
		CHECK(int16_t(decode_uint16(pcm.ptr() + i * 4 + 2)) == -1000);
	}
	const double json_loop_begin = 32.123;
	const double json_loop_end = 33.234;
	wav->set_loop_begin(int(Math::round(json_loop_begin * wav->get_mix_rate())));
	wav->set_loop_end(int(Math::round(json_loop_end * wav->get_mix_rate())));
	wav->set_loop_mode(AudioStreamWAV::LOOP_FORWARD);
	CHECK(wav->get_loop_begin() == 1541904);
	CHECK(wav->get_loop_end() == 1595232);
	CHECK(wav->get_loop_end() < frames);
	CHECK(json_loop_begin == 32.123);
}

TEST_CASE("[PCMDecode] The PCM cap fails without a partial stream and permits an exact-fit buffer") {
	initialize_audio();
	const String path = wave_file(4097, 48000);
	for (bool exact : { false, true }) {
		Decode job;
		job.path = path;
		job.limit = (exact ? 4097 : 4096) * 4;
		job.deadline = OS::get_singleton()->get_ticks_usec() + 10000000;
		job.execute();
		if (exact) {
			CHECK(job.error == OK);
			CHECK(int64_t(job.result["pcm_bytes"]) == job.limit);
		} else {
			CHECK(job.error == ERR_OUT_OF_MEMORY);
			CHECK_FALSE(job.result.has("stream"));
		}
	}
}

TEST_CASE("[PCMDecode] Resampling drains to actual output frames without appending a realtime block tail") {
	initialize_audio();
	Decode job;
	job.path = wave_file(48000, 48000);
	job.rate = 24000;
	job.deadline = OS::get_singleton()->get_ticks_usec() + 10000000;
	job.execute();
	REQUIRE(job.error == OK);
	CHECK(int64_t(job.result["frame_count"]) == 24000);
	CHECK(int64_t(job.result["pcm_bytes"]) == 24000 * 4);
	CHECK(double(job.result["duration"]) == doctest::Approx(1.0));
	Ref<AudioStreamWAV> wav = job.result["stream"];
	REQUIRE(wav.is_valid());
	CHECK(wav->get_mix_rate() == 24000);
}

TEST_CASE("[PCMDecode] Cancellation and an expired total deadline never expose partial decoded audio") {
	for (bool cancel : { true, false }) {
		Decode job;
		job.path = "unused-because-request-is-interrupted.wav";
		job.deadline = OS::get_singleton()->get_ticks_usec() + (cancel ? 1000000 : 0);
		if (cancel) {
			job.cancelled.set();
		}
		job.execute();
		CHECK(job.error == (cancel ? ERR_SKIP : ERR_TIMEOUT));
		CHECK_FALSE(job.result.has("stream"));
	}
}

TEST_CASE("[PCMDecode][WAVLoop] Forward looping excludes loop_end and includes loop_begin at every seam") {
	initialize_audio();
	Ref<AudioStreamWAV> wav;
	wav.instantiate();
	wav->set_format(AudioStreamWAV::FORMAT_16_BITS);
	wav->set_stereo(true);
	wav->set_mix_rate(AudioServer::get_singleton()->get_mix_rate());
	Vector<uint8_t> pcm;
	pcm.resize(8 * 4);
	for (int i = 0; i < 8; i++) {
		encode_uint16(i + 1, pcm.ptrw() + i * 4);
		encode_uint16(i + 1, pcm.ptrw() + i * 4 + 2);
	}
	wav->set_data(pcm);
	wav->set_loop_begin(2);
	wav->set_loop_end(5);
	wav->set_loop_mode(AudioStreamWAV::LOOP_FORWARD);
	Ref<AudioStreamPlayback> playback = wav->instantiate_playback();
	playback->start();
	AudioFrame mixed[17];
	REQUIRE(playback->mix(mixed, 1.0, 17) == 17);
	for (int i = 0; i < 17; i++) {
		// The standard cubic resampler has two frames of startup lookbehind.
		const int index = i - 2;
		const int source = index < 5 ? index : 2 + (index - 5) % 3;
		const float expected = index < 0 ? 0.0f : float(source + 1) / 32767.0f;
		CHECK(mixed[i].left == doctest::Approx(expected).epsilon(0.000001));
		CHECK(mixed[i].right == doctest::Approx(expected).epsilon(0.000001));
	}
	playback->stop();
}
} // namespace TestPCMDecode
#endif
