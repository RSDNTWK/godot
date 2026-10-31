/**************************************************************************/
/*  test_ffmpeg_local_policy.cpp                                      */
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
TEST_FORCE_LINK(test_ffmpeg_local_policy)
#include "modules/modules_enabled.gen.h"
#ifdef MODULE_FFMPEG_ENABLED
#include "core/io/file_access_pack.h"
#include "core/io/dir_access.h"
#include "core/config/project_settings.h"
#include "core/io/pck_packer.h"
#include "core/os/os.h"
#include "modules/ffmpeg/ffmpeg_file_io.h"
#include "modules/ffmpeg/video_stream_media.h"
#include "tests/test_utils.h"

namespace TestFFmpegLocalPolicy {
TEST_CASE("[R4][FFmpegPolicy] Direct network inputs fail before allocating a demuxer") {
	for (const char *path : { "http://127.0.0.1:9/media.mp3", "tcp://127.0.0.1:9", "https://127.0.0.1:9/media.mkv", "HTTP://127.0.0.1:9/media.mp3", "http:127.0.0.1:9/media.mp3", "file://127.0.0.1/share/media.mov", "\\\\127.0.0.1\\share\\media.mov", "\\/127.0.0.1/share/media.mov", "/\\127.0.0.1/share/media.mov" }) {
		FFmpegFileIO input;
		AVFormatContext *format = nullptr;
		CHECK(FFmpegFileIO::is_remote_path(path));
		CHECK(input.open(&format, path) == AVERROR_PROTOCOL_NOT_FOUND);
		CHECK(format == nullptr);
	}
	for (const char *path : { "res://media/test.mp3", "user://evidence_cache/test.mp4", "uid://test", "C:/media/test.mkv", "/tmp/media.opus" }) {
		CHECK_FALSE(FFmpegFileIO::is_remote_path(path));
	}
}
TEST_CASE("[R4][FFmpegPolicy] Secondary provider opens retain the file-only whitelist") {
	AVFormatContext *unrelated = avformat_alloc_context();
	REQUIRE(unrelated != nullptr);
	const auto original_open = unrelated->io_open;
	CHECK(unrelated->protocol_whitelist == nullptr);
	AVFormatContext *format = avformat_alloc_context();
	REQUIRE(format != nullptr);
	format->url = av_strdup("memory");
	REQUIRE(format->url != nullptr);
	REQUIRE(FFmpegFileIO::configure_local_input(format) == 0);
	// An accidental whitelist regression must not attempt network IO in this test.
	format->interrupt_callback = { [](void *) { return 1; }, nullptr };
	for (const char *url : { "http://127.0.0.1:9/media.mp3", "tcp://127.0.0.1:9", "C:/media/external.raw", "/tmp/external.raw" }) {
		AVIOContext *io = nullptr;
		AVDictionary *options = nullptr;
		CHECK(format->io_open(format, &io, url, AVIO_FLAG_READ, &options) == AVERROR_PROTOCOL_NOT_FOUND);
		CHECK(io == nullptr);
		av_dict_free(&options);
		if (io) { avio_close(io); }
	}
	CHECK(unrelated->io_open == original_open);
	CHECK(unrelated->protocol_whitelist == nullptr);
	avformat_free_context(unrelated);
	avformat_free_context(format);
}
static void decode_local(const String &p_path) {
	FFmpegFileIO input;
	AVFormatContext *format = nullptr;
	REQUIRE(input.open(&format, p_path) >= 0);
	REQUIRE(format != nullptr);
	CHECK(avformat_find_stream_info(format, nullptr) >= 0);
	CHECK(av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) >= 0);
	avformat_close_input(&format);
	input.close();
	Ref<AudioStreamMedia> stream;
	stream.instantiate();
	stream->set_file(p_path);
	CHECK(stream->get_length() > 0.0);
	Ref<AudioStreamPlayback> playback = stream->instantiate_playback();
	REQUIRE(playback.is_valid());
	playback->start();
	AudioFrame output[128];
	CHECK(playback->mix(output, 1.0, 128) > 0);
	playback->seek(0.005);
	CHECK(playback->mix(output, 1.0, 128) > 0);
	CHECK(playback->get_playback_position() >= 0.005);
	playback->stop();
}
TEST_CASE("[Audio][R4][FFmpegPolicy] Downloaded files and PCK virtual files still decode") {
	const Vector<uint8_t> media = FileAccess::get_file_as_bytes(TestUtils::get_data_path("media/native_tone_44100.mp3"));
	REQUIRE_FALSE(media.is_empty());
	const String suffix = uitos(OS::get_singleton()->get_ticks_usec());
	const String local = TestUtils::get_temp_path("r4_download_" + suffix + ".mp3");
	{
		Ref<FileAccess> output = FileAccess::open(local, FileAccess::WRITE);
		REQUIRE(output.is_valid());
		if (output.is_null()) { return; }
		REQUIRE(output->store_buffer(media));
	}
	decode_local(local);
	{
		const String previous = TestProjectSettingsInternalsAccessor::resource_path();
		TestProjectSettingsInternalsAccessor::resource_path() = TestUtils::get_executable_dir().path_join("..").simplify_path();
		decode_local("res://tests/data/media/native_tone_44100.mp3");
		TestProjectSettingsInternalsAccessor::resource_path() = previous;
	}
	const Error user_directory = DirAccess::make_dir_recursive_absolute(OS::get_singleton()->get_user_data_dir());
	REQUIRE(user_directory == OK);
	const String user_path = "user://r4_download_" + suffix + ".mp3";
	{
		Ref<FileAccess> output = FileAccess::open(user_path, FileAccess::WRITE);
		REQUIRE(output.is_valid());
		if (output.is_null()) { return; }
		REQUIRE(output->store_buffer(media));
	}
	decode_local(user_path);
	const String pack = TestUtils::get_temp_path("r4_local_" + suffix + ".pck");
	const String virtual_path = "res://r4_local_" + suffix + "/media.mp3";
	{
		PCKPacker packer;
		REQUIRE(packer.pck_start(pack) == OK);
		REQUIRE(packer.add_file_from_buffer(virtual_path, media) == OK);
		REQUIRE(packer.flush() == OK);
	}
	REQUIRE(PackedData::get_singleton()->add_pack(pack, false, 0) == OK);
	decode_local(virtual_path);
	FFmpegAudioMetadata metadata;
	CHECK(ffmpeg_probe_audio_buffer(media, metadata));
	CHECK(metadata.sample_rate > 0);
	CHECK(metadata.channels > 0);
	Ref<AudioStreamPlayback> memory_playback = ffmpeg_audio_playback_from_buffer(media);
	REQUIRE(memory_playback.is_valid());
	if (memory_playback.is_valid()) {
		memory_playback->start();
		AudioFrame frames[128];
		CHECK(memory_playback->mix(frames, 1.0, 128) > 0);
		memory_playback->seek(0.005);
		CHECK(memory_playback->mix(frames, 1.0, 128) > 0);
		CHECK(memory_playback->get_playback_position() >= 0.005);
		memory_playback->stop();
	}
}

struct AliasInput {
	Ref<FileAccess> file;
	static int read(void *p_self, uint8_t *p_buffer, int p_count) {
		AliasInput *self = static_cast<AliasInput *>(p_self);
		const uint64_t count = self->file->get_buffer(p_buffer, p_count);
		return count ? int(count) : AVERROR_EOF;
	}
	static int64_t seek(void *p_self, int64_t p_offset, int p_whence) {
		AliasInput *self = static_cast<AliasInput *>(p_self);
		const int64_t length = self->file->get_length();
		if (p_whence & AVSEEK_SIZE) { return length; }
		p_whence &= ~AVSEEK_FORCE;
		const int64_t base = p_whence == SEEK_CUR ? int64_t(self->file->get_position()) : p_whence == SEEK_END ? length : 0;
		if (p_offset < -base || p_offset > length - base) { return AVERROR(EINVAL); }
		self->file->seek(base + p_offset);
		return self->file->get_position();
	}
};
struct AliasObservation {
	using Open = int (*)(AVFormatContext *, AVIOContext **, const char *, int, AVDictionary **);
	Open configured = nullptr;
	int requests = 0;
	String path;
	int result = 0;
	bool returned_io = false;
};
static thread_local AliasObservation *alias_observer = nullptr;
static int observe_alias(AVFormatContext *p_format, AVIOContext **r_io, const char *p_path, int p_flags, AVDictionary **p_options) {
	alias_observer->requests++;
	alias_observer->path = p_path;
	alias_observer->result = alias_observer->configured(p_format, r_io, p_path, p_flags, p_options);
	alias_observer->returned_io = *r_io != nullptr;
	return alias_observer->result;
}
TEST_CASE("[R4][FFmpegPolicy] A real local MOV external track is rejected before its remote resource opens") {
	const String path = TestUtils::get_data_path("media/r4_remote_reference.mov");
	for (bool enabled : { false, true }) {
		AliasInput input;
		input.file = FileAccess::open(path, FileAccess::READ);
		REQUIRE(input.file.is_valid());
		if (input.file.is_null()) { return; }
		uint8_t *buffer = static_cast<uint8_t *>(av_malloc(32768));
		REQUIRE(buffer != nullptr);
		AVIOContext *io = avio_alloc_context(buffer, 32768, 0, &input, AliasInput::read, nullptr, AliasInput::seek);
		REQUIRE(io != nullptr);
		AVFormatContext *format = avformat_alloc_context();
		REQUIRE(format != nullptr);
		REQUIRE(FFmpegFileIO::configure_local_input(format) == 0);
		AliasObservation observation;
		observation.configured = format->io_open;
		alias_observer = &observation;
		format->io_open = observe_alias;
		format->pb = io;
		format->flags |= AVFMT_FLAG_CUSTOM_IO;
		AVDictionary *options = nullptr;
		REQUIRE(av_dict_set(&options, "enable_drefs", enabled ? "1" : "0", 0) == 0);
		REQUIRE(av_dict_set(&options, "use_absolute_path", "1", 0) == 0);
		const int opened = avformat_open_input(&format, path.utf8().get_data(), nullptr, &options);
		av_dict_free(&options);
		CHECK(opened >= 0);
		CHECK(observation.requests == (enabled ? 1 : 0));
		if (enabled) {
			CHECK(observation.path == "\\\\127.0.0.1\\godot-r4-test\\external.raw");
			CHECK(observation.result == AVERROR_PROTOCOL_NOT_FOUND);
			CHECK_FALSE(observation.returned_io);
		}
		if (format) {
			AVPacket *packet = av_packet_alloc();
			REQUIRE(packet != nullptr);
			CHECK(av_read_frame(format, packet) == AVERROR_EOF);
			av_packet_free(&packet);
		}
		avformat_close_input(&format);
		alias_observer = nullptr;
		av_freep(&io->buffer);
		avio_context_free(&io);
	}
}
TEST_CASE("[Audio][R4][FFmpegPolicy] Self-contained MOV control still plays and seeks") {
	decode_local(TestUtils::get_data_path("media/r4_self_contained.mov"));
}

TEST_CASE("[Audio][R4][FFmpegPolicy] Rejected URLs surface as failed public playback") {
	const bool previous = CoreGlobals::print_error_enabled;
	CoreGlobals::print_error_enabled = false;
	for (const char *path : { "http://127.0.0.1:9/media.mp3", "http://127.0.0.1:9/media.aac", "\\\\127.0.0.1\\share\\media.aac", "\\/127.0.0.1/share/media.aac", "/\\127.0.0.1/share/media.aac" }) {
		Ref<AudioStreamMedia> audio;
		audio.instantiate();
		audio->set_file(path);
		CHECK(audio->get_length() == 0.0);
		CHECK(audio->instantiate_playback().is_null());
	}
	Ref<VideoStreamMedia> video;
	video.instantiate();
	video->set_file("http://127.0.0.1:9/media.mp4");
	CHECK(video->instantiate_playback().is_null());
	CoreGlobals::print_error_enabled = previous;
}
struct VideoObservation {
	double before_seek = 0.0;
	double after_seek = 0.0;
	int audio_frames = 0;
};
static VideoObservation decode_video(const String &p_path) {
	VideoObservation observed;
	Ref<VideoStreamMedia> stream;
	stream.instantiate();
	stream->set_file(p_path);
	Ref<VideoStreamPlayback> playback = stream->instantiate_playback();
	REQUIRE(playback.is_valid());
	if (playback.is_null()) { return observed; }
	playback->set_mix_callback([](void *p_state, const float *, int p_frames) {
		*static_cast<int *>(p_state) += p_frames;
		return p_frames;
	}, &observed.audio_frames);
	playback->play();
	for (int i = 0; i < 12; i++) { playback->update(1.0 / 240.0); }
	observed.before_seek = playback->get_playback_position();
	CHECK(observed.before_seek > 0.0);
	CHECK(playback->get_texture().is_valid());
	if (playback->get_texture().is_valid()) { CHECK(playback->get_texture()->get_width() > 0); }
	playback->set_paused(true);
	playback->update(0.02);
	CHECK(playback->get_playback_position() == observed.before_seek);
	playback->seek(0.13);
	playback->set_paused(false);
	for (int i = 0; i < 12; i++) { playback->update(1.0 / 240.0); }
	observed.after_seek = playback->get_playback_position();
	CHECK(observed.after_seek >= 0.07);
	CHECK(observed.after_seek < 0.3);
	CHECK(observed.audio_frames > 0);
	playback->stop();
	return observed;
}
TEST_CASE("[Audio][R4][FFmpegPolicy] Local virtual video paths retain the same audio and video clocks") {
	const String absolute = TestUtils::get_data_path("media/ffmpeg_origin0.mkv");
	const Vector<uint8_t> media = FileAccess::get_file_as_bytes(absolute);
	REQUIRE_FALSE(media.is_empty());
	const String suffix = uitos(OS::get_singleton()->get_ticks_usec());
	REQUIRE(DirAccess::make_dir_recursive_absolute(OS::get_singleton()->get_user_data_dir()) == OK);
	const String user = "user://r4_video_" + suffix + ".mkv";
	{
		Ref<FileAccess> output = FileAccess::open(user, FileAccess::WRITE);
		REQUIRE(output.is_valid());
		if (output.is_null()) { return; }
		REQUIRE(output->store_buffer(media));
	}
	const String pack = TestUtils::get_temp_path("r4_video_" + suffix + ".pck");
	const String packed = "res://r4_video_" + suffix + "/media.mkv";
	{
		PCKPacker packer;
		REQUIRE(packer.pck_start(pack) == OK);
		REQUIRE(packer.add_file_from_buffer(packed, media) == OK);
		REQUIRE(packer.flush() == OK);
	}
	REQUIRE(PackedData::get_singleton()->add_pack(pack, false, 0) == OK);
	struct ResourcePathScope {
		String previous = TestProjectSettingsInternalsAccessor::resource_path();
		ResourcePathScope() { TestProjectSettingsInternalsAccessor::resource_path() = TestUtils::get_executable_dir().path_join("..").simplify_path(); }
		~ResourcePathScope() { TestProjectSettingsInternalsAccessor::resource_path() = previous; }
	} resource_path;
	const VideoObservation reference = decode_video(absolute);
	for (const String &path : { String("res://tests/data/media/ffmpeg_origin0.mkv"), user, packed }) {
		const VideoObservation observed = decode_video(path);
		CHECK(observed.before_seek == doctest::Approx(reference.before_seek).epsilon(0.0001));
		CHECK(observed.after_seek == doctest::Approx(reference.after_seek).epsilon(0.0001));
		CHECK(observed.audio_frames == reference.audio_frames);
	}
}
} // namespace TestFFmpegLocalPolicy
#endif
