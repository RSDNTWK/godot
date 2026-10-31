/**************************************************************************/
/*  test_native_webm_fallback.cpp                                          */
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
TEST_FORCE_LINK(test_native_webm_fallback)
#include "modules/modules_enabled.gen.h"
#ifdef MODULE_WEBM_ENABLED
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "servers/audio/audio_server.h"
#include "tests/core/io/allocation_test_helpers.h"
#include "tests/test_utils.h"

#include "modules/webm/video_stream_webm.h"

#include <cstring>
#ifndef MODULE_FFMPEG_ENABLED
#include "thirdparty/libsimplewebm/OpusVorbisDecoder.hpp"
#include "thirdparty/libsimplewebm/VPXDecoder.hpp"
#include "thirdparty/libsimplewebm/libwebm/mkvparser/mkvparser.h"
#endif
namespace TestNativeWebm {
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
struct Quiet {
	bool previous = CoreGlobals::print_error_enabled;
	Quiet() { CoreGlobals::print_error_enabled = false; }
	~Quiet() { CoreGlobals::print_error_enabled = previous; }
};
static Ref<VideoStreamPlayback> playback(const String &p_path) {
	Ref<VideoStreamWebm> stream;
	stream.instantiate();
	stream->set_file(p_path);
	return stream->instantiate_playback();
}
struct AudioCapture {
	int calls = 0;
	int frames = 0;
	bool partial = false;
	bool nonzero = false;
};
static int mix(void *p_userdata, const float *p_buffer, int p_frames) {
	AudioCapture &capture = *static_cast<AudioCapture *>(p_userdata);
	++capture.calls;
	capture.frames += p_frames;
	for (int i = 0; i < p_frames * 2; ++i) {
		CHECK(Math::is_finite(p_buffer[i]));
		capture.nonzero |= p_buffer[i] != 0;
	}
	return capture.partial ? 0 : p_frames;
}
TEST_CASE("[SceneTree][F25][NativeWebM] Factory plays local VP8 Vorbis VP9 Opus and silent media") {
	AudioContext audio;
	SoftwareEnvironment environment;
	for (const char *name : { "r4_native_vp8_vorbis.webm", "r4_native_vp9_opus.webm", "r4_native_vp8_silent.webm" }) {
		Ref<VideoStreamPlayback> player = playback(TestUtils::get_data_path(String("media/") + name));
		REQUIRE(player.is_valid());
		if (player.is_null()) {
			continue;
		}
		REQUIRE(player->get_texture().is_valid());
		CHECK(player->get_texture()->get_width() == 32);
		CHECK(player->get_texture()->get_height() == 16);
		CHECK(player->get_length() >= 0.3);
		CHECK(player->get_length() < 0.6);
		const bool silent = String(name).contains("silent");
		CHECK(player->get_channels() == (silent ? 0 : 2));
		AudioCapture capture;
		player->set_mix_callback(mix, &capture);
		player->play();
		CHECK(player->is_playing());
		player->set_paused(true);
		const double position = player->get_playback_position();
		player->update(0.1);
		CHECK(player->get_playback_position() == position);
		player->set_paused(false);
		for (int i = 0; i < 240 && player->is_playing(); ++i) {
			player->update(1.0 / 240);
		}
		CHECK_FALSE(player->is_playing());
		if (!silent) {
			CHECK(capture.calls > 0);
			CHECK(capture.frames > 0);
			CHECK(capture.nonzero);
		}
		player->play();
		CHECK(player->is_playing());
		player->update(0.01);
		player->stop();
		CHECK_FALSE(player->is_playing());
		player->play();
		CHECK(player->is_playing());
		player->stop();
	}
}
#ifndef MODULE_FFMPEG_ENABLED
TEST_CASE("[SceneTree][F25][NativeWebM] Failed opens and failed Vorbis initialization remain safe") {
	Quiet quiet;
	CHECK(playback(TestUtils::get_temp_path("missing-native-video.webm")).is_null());
	const String scratch = TestUtils::get_temp_path("invalid-native-video.webm");
	Ref<FileAccess> file = FileAccess::open(scratch, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	if (file.is_null()) {
		return;
	}
	file->store_string("invalid WebM");
	file.unref();
	CHECK(playback(scratch).is_null());
	Vector<uint8_t> data = FileAccess::get_file_as_bytes(TestUtils::get_data_path("media/r4_native_vp8_vorbis.webm"));
	const uint8_t signature[] = { 1, 'v', 'o', 'r', 'b', 'i', 's' };
	int64_t header = -1;
	for (int64_t i = 0; i + 12 < data.size(); ++i) {
		if (memcmp(data.ptr() + i, signature, sizeof(signature)) == 0) {
			header = i;
			break;
		}
	}
	REQUIRE(header >= 0);
	if (header < 0) {
		return;
	}
	data.write[header + 11] = 0; // Invalid channel count in the identification header.
	file = FileAccess::open(scratch, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	if (file.is_null()) {
		return;
	}
	REQUIRE(file->store_buffer(data));
	file.unref();
	for (int i = 0; i < 3; ++i) {
		Ref<VideoStreamPlayback> player = playback(scratch);
		REQUIRE(player.is_valid());
		if (player.is_null()) {
			continue;
		}
		CHECK(player->get_channels() == 0);
		player->play();
		player->update(0.01);
		player->stop();
	}
}
TEST_CASE("[SceneTree][F25][NativeWebM] Clearing a partial audio callback and failed restart remain safe") {
	Quiet quiet;
	const String scratch = TestUtils::get_temp_path("restart-native-video.webm");
	Vector<uint8_t> data = FileAccess::get_file_as_bytes(TestUtils::get_data_path("media/r4_native_vp8_vorbis.webm"));
	Ref<FileAccess> file = FileAccess::open(scratch, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	if (file.is_null()) {
		return;
	}
	REQUIRE(file->store_buffer(data));
	file.unref();
	Ref<VideoStreamPlayback> player = playback(scratch);
	REQUIRE(player.is_valid());
	if (player.is_null()) {
		return;
	}
	AudioCapture capture;
	capture.partial = true;
	player->set_mix_callback(mix, &capture);
	player->play();
	for (int i = 0; i < 30 && capture.calls == 0; ++i) {
		player->update(1.0 / 240);
	}
	REQUIRE(capture.calls > 0);
	const int calls = capture.calls;
	player->set_mix_callback(nullptr, nullptr);
	for (int i = 0; i < 10; ++i) {
		player->update(1.0 / 240);
	}
	CHECK(capture.calls == calls);
	file = FileAccess::open(scratch, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	if (file.is_null()) {
		return;
	}
	file->store_string("truncated");
	file.unref();
	player->stop();
	CHECK_FALSE(player->is_playing());
	player->play();
	CHECK_FALSE(player->is_playing());
}
#ifdef B01_ALLOCATION_TESTS
TEST_CASE("[SceneTree][F25][NativeWebM] Frame buffer allocation failure leaves no playback") {
	Quiet quiet;
	const String path = TestUtils::get_data_path("media/r4_native_vp8_vorbis.webm");
	const size_t allocation = TestAllocation::allocation_size([]() { Vector<uint8_t> probe; REQUIRE(probe.resize(32 * 16 * 4) == OK); });
	REQUIRE(allocation > 0);
	TestAllocation::Failure fault(allocation);
	CHECK(playback(path).is_null());
	CHECK(TestAllocation::failures() == 1);
}
#endif
#endif

#ifndef MODULE_FFMPEG_ENABLED

TEST_CASE("[SceneTree][F25][NativeWebM] Pause persists across restart and audio format cannot change under the callback") {
	Quiet quiet;
	Ref<VideoStreamPlayback> player = playback(TestUtils::get_data_path("media/r4_native_vp8_tracks.webm"));
	REQUIRE(player.is_valid());
	if (player.is_null()) {
		return;
	}
	CHECK(player->get_channels() == 2);
	CHECK(player->get_mix_rate() == 44100);
	AudioCapture capture;
	player->set_mix_callback(mix, &capture);
	player->play();
	player->set_paused(true);
	player->stop();
	CHECK(player->is_paused());
	player->play();
	CHECK(player->is_paused());
	CHECK(player->is_playing());
	const double position = player->get_playback_position();
	player->update(0.2);
	CHECK(player->get_playback_position() == position);
	player->set_paused(false);
	for (int track : { 1, 2 }) {
		player->set_audio_track(track);
		player->stop();
		CHECK_FALSE(player->is_playing());
		player->play();
		CHECK_FALSE(player->is_playing());
		player->update(0.1);
		player->set_audio_track(0);
		player->play();
		CHECK(player->is_playing());
		CHECK(player->get_channels() == 2);
		CHECK(player->get_mix_rate() == 44100);
	}
	player->set_audio_track(3);
	player->stop();
	player->play();
	CHECK(player->is_playing());
	CHECK(player->get_channels() == 2);
	CHECK(player->get_mix_rate() == 44100);
	for (int i = 0; i < 100 && capture.calls == 0; ++i) {
		player->update(1.0 / 240);
	}
	CHECK(capture.calls > 0);
	player->stop();
}
class MemoryReader : public mkvparser::IMkvReader {
	Vector<uint8_t> data;

public:
	explicit MemoryReader(const Vector<uint8_t> &p_data) : data(p_data) {}
	int Read(long long p_position, long p_length, unsigned char *p_buffer) override {
		if (p_position < 0 || p_length < 0 || uint64_t(p_position) > uint64_t(data.size()) || uint64_t(p_length) > uint64_t(data.size()) - uint64_t(p_position)) {
			return -1;
		}
		memcpy(p_buffer, data.ptr() + p_position, p_length);
		return 0;
	}
	int Length(long long *r_total, long long *r_available) override {
		if (r_total) {
			*r_total = data.size();
		}
		if (r_available) {
			*r_available = data.size();
		}
		return 0;
	}
};
TEST_CASE("[F25][NativeWebM] Native demux and decode produce bounded pixels and timestamped audio") {
	for (const char *name : { "r4_native_vp8_vorbis.webm", "r4_native_vp9_opus.webm" }) {
		const Vector<uint8_t> data = FileAccess::get_file_as_bytes(TestUtils::get_data_path(String("media/") + name));
		WebMDemuxer demuxer(new MemoryReader(data));
		REQUIRE(demuxer.isOpen());
		if (!demuxer.isOpen()) {
			continue;
		}
		VPXDecoder video(demuxer, 1);
		OpusVorbisDecoder audio(demuxer);
		REQUIRE(video.isOpen());
		REQUIRE(audio.isOpen());
		if (!video.isOpen() || !audio.isOpen()) {
			continue;
		}
		REQUIRE(demuxer.getChannels() == 2);
		Vector<float> pcm;
		REQUIRE(pcm.resize(audio.getBufferSamples() * demuxer.getChannels()) == OK);
		WebMFrame vf, af;
		int images = 0, samples = 0;
		double video_time = -1, audio_time = -1;
		for (int packet = 0; packet < 100 && demuxer.readFrame(&vf, &af); ++packet) {
			if (vf.isValid()) {
				CHECK(vf.time >= video_time);
				video_time = vf.time;
				REQUIRE(video.decode(vf));
				VPXDecoder::Image image;
				if (video.getImage(image) == VPXDecoder::NO_ERROR) {
					++images;
					CHECK(image.w == 32);
					CHECK(image.h == 16);
					for (int plane = 0; plane < 3; ++plane) {
						CHECK(image.planes[plane] != nullptr);
						CHECK(image.linesize[plane] > 0);
					}
				}
			}
			if (af.isValid()) {
				CHECK(af.time >= audio_time);
				audio_time = af.time;
				int frames = 0;
				REQUIRE(audio.getPCMF(af, pcm.ptrw(), frames));
				CHECK(frames >= 0);
				CHECK(frames <= audio.getBufferSamples());
				if (frames > 0 && frames <= audio.getBufferSamples()) {
					samples += frames;
					for (int i = 0; i < frames * 2; ++i) {
						CHECK(Math::is_finite(pcm[i]));
					}
				}
			}
		}
		CHECK(demuxer.isEOS());
		CHECK(images > 0);
		CHECK(samples > 0);
		CHECK(Math::abs(video_time - audio_time) < 0.1);
	}
}
#endif
} // namespace TestNativeWebm
#endif
