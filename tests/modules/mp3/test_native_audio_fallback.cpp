/**************************************************************************/
/*  test_native_audio_fallback.cpp                                       */
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

TEST_FORCE_LINK(test_native_audio_fallback)

#include "modules/modules_enabled.gen.h"
#if defined(MODULE_MP3_ENABLED) && defined(MODULE_VORBIS_ENABLED) && !defined(MODULE_FFMPEG_ENABLED)
#include "servers/audio/audio_server.h"
#include "tests/test_utils.h"

#include "modules/mp3/audio_stream_mp3.h"
#include "modules/vorbis/audio_stream_ogg_vorbis.h"

namespace TestNativeAudioFallback {
static void check_playback(const Ref<AudioStream> &p_stream, const StringName &p_resource_class, const StringName &p_playback_class) {
	REQUIRE(p_stream.is_valid());
	if (p_stream.is_null()) {
		return;
	}
	CHECK(p_stream->get_class() == p_resource_class);
	CHECK(p_stream->get_length() > 0.9);
	CHECK(p_stream->get_length() < 1.2);
	Ref<AudioStreamPlayback> playback = p_stream->instantiate_playback();
	REQUIRE(playback.is_valid());
	if (playback.is_null()) {
		return;
	}
	CHECK(playback->get_class() == p_playback_class);
	AudioFrame frames[512];
	playback->start_playback();
	CHECK(playback->is_playing());
	CHECK(playback->mix(frames, 1.0f, 512) == 512);
	float peak = 0.0f;
	for (const AudioFrame &frame : frames) {
		peak = MAX(peak, MAX(Math::abs(frame.left), Math::abs(frame.right)));
	}
	CHECK(peak > 0.01f);
	playback->stop_playback();
	CHECK_FALSE(playback->is_playing());
	playback->start_playback(0.4);
	CHECK(playback->mix(frames, 1.0f, 512) == 512);
	CHECK(playback->get_playback_position() > 0.39);
	CHECK(playback->get_playback_position() < 0.48);
	playback->seek_playback(0.2);
	CHECK(playback->mix(frames, 1.0f, 512) == 512);
	CHECK(playback->get_playback_position() > 0.19);
	CHECK(playback->get_playback_position() < 0.28);

	Ref<AudioSamplePlayback> sample;
	sample.instantiate();
	playback->set_is_sample(true);
	playback->set_sample_playback(sample);
	CHECK(playback->get_is_sample());
	CHECK(playback->get_sample_playback() == sample);
	CHECK(sample->stream_playback == playback);
	playback->set_sample_playback(nullptr);
	CHECK(playback->get_sample_playback().is_null());
	sample->stream_playback.unref();
	playback->set_is_sample(false);

	playback->start_playback();
	int total = 0;
	for (int batch = 0; batch < 200 && playback->is_playing(); batch++) {
		const int mixed = playback->mix(frames, 1.0f, 512);
		REQUIRE(mixed >= 0);
		REQUIRE(mixed <= 512);
		total += mixed;
	}
	CHECK_FALSE(playback->is_playing());
	CHECK(total > int(AudioServer::get_singleton()->get_mix_rate() * 0.9));
	CHECK(total < int(AudioServer::get_singleton()->get_mix_rate() * 1.2));
	playback->stop_playback();
}

TEST_CASE("[Audio][F25][NativeAudio] Typed MP3 retains native playback seek restart sample hooks and EOF") {
	Ref<AudioStreamMP3> stream = AudioStreamMP3::load_from_file(TestUtils::get_data_path("media/native_tone_44100.mp3"));
	check_playback(stream, "AudioStreamMP3", "AudioStreamPlaybackMP3");
}

TEST_CASE("[Audio][F25][NativeAudio][N018] Typed Vorbis retains native playback seek restart sample hooks and EOF") {
	Ref<AudioStreamOggVorbis> stream = AudioStreamOggVorbis::load_from_file(TestUtils::get_data_path("media/ffmpeg_tone_44100.ogg"));
	check_playback(stream, "AudioStreamOggVorbis", "AudioStreamPlaybackOggVorbis");
}
} // namespace TestNativeAudioFallback
#endif
