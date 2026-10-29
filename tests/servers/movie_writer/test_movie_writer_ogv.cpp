/**************************************************************************/
/*  test_movie_writer_ogv.cpp                                           */
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

TEST_FORCE_LINK(test_movie_writer_ogv)

#include "modules/modules_enabled.gen.h"
#if defined(MODULE_THEORA_ENABLED) && defined(DEV_ENABLED)
#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "servers/audio/audio_driver_dummy.h"
#include "tests/test_utils.h"

#include "modules/theora/editor/movie_writer_ogv.h"
#ifdef MODULE_FFMPEG_ENABLED
#include "modules/ffmpeg/movie_writer_ogv_ffmpeg.h"
#endif
#if defined(LINUXBSD_ENABLED) && defined(UNIX_ENABLED)
#include <sys/resource.h>

#include <csignal>
#endif

namespace TestMovieWriterOGV {
struct Settings {
	int previous_driver_rate =
			AudioDriverDummy::get_dummy_singleton()->get_mix_rate();
	AudioDriver::SpeakerMode previous_driver_mode =
			AudioDriverDummy::get_dummy_singleton()->get_speaker_mode();
	const char *names[7] = { "editor/movie_writer/mix_rate",
		"editor/movie_writer/speaker_mode",
		"editor/movie_writer/video_quality",
		"editor/movie_writer/ogv/audio_quality",
		"editor/movie_writer/ogv/encoding_speed",
		"editor/movie_writer/ogv/keyframe_interval",
		"application/config/name" };
	Variant previous[7];
	Settings() {
		for (int i = 0; i < 7; i++) {
			previous[i] = ProjectSettings::get_singleton()->get(names[i]);
		}
		const Variant values[7] = {
			48000, 0, 0.75, 0.5, 4, 64, "Recording contract controls"
		};
		for (int i = 0; i < 7; i++) {
			ProjectSettings::get_singleton()->set(names[i], values[i]);
		}
	}
	~Settings() {
		AudioDriverDummy::get_dummy_singleton()->set_mix_rate(previous_driver_rate);
		AudioDriverDummy::get_dummy_singleton()->set_speaker_mode(
				previous_driver_mode);
		for (int i = 0; i < 7; i++) {
			ProjectSettings::get_singleton()->set(names[i], previous[i]);
		}
	}
	void rate(int p_rate) {
		ProjectSettings::get_singleton()->set(names[0], p_rate);
	}
	void mode(int p_mode) {
		ProjectSettings::get_singleton()->set(names[1], p_mode);
	}
};
template <typename T>
struct Owned {
	T *writer = memnew(T);
	~Owned() { memdelete(writer); }
	T *operator->() { return writer; }
};
struct Native : MovieWriterOGV {
	using MovieWriterOGV::write_begin;
	using MovieWriterOGV::write_end;
	using MovieWriterOGV::write_frame;
};
#ifdef MODULE_FFMPEG_ENABLED
struct FFmpeg : MovieWriterOGVFFmpeg {
	using MovieWriterOGVFFmpeg::write_begin;
	using MovieWriterOGVFFmpeg::write_end;
	using MovieWriterOGVFFmpeg::write_frame;
};
#endif
static String output(const String &p_name) {
	const String directory =
			OS::get_singleton()->get_environment("GODOT_RECORDING_TEST_DIR");
	return directory.is_empty() ? TestUtils::get_temp_path(p_name)
								: directory.path_join(p_name);
}
static Ref<Image> image(Image::Format p_format = Image::FORMAT_RGBA8) {
	Ref<Image> result = Image::create_empty(18, 18, false, p_format);
	if (result.is_valid() && !result->is_empty()) {
		result->fill(Color(0.25, 0.5, 0.75, 1));
	}
	return result;
}
static Vector<int32_t> sound(int p_samples, int p_channels) {
	Vector<int32_t> result;
	if (result.resize(p_samples * p_channels) != OK) {
		return result;
	}
	int32_t *data = result.ptrw();
	for (int i = 0; i < p_samples; i++) {
		for (int j = 0; j < p_channels; j++) {
			data[i * p_channels + j] = ((i % 257) - 128) * 1048576;
		}
	}
	return result;
}
template <typename T>
static bool record(const String &p_name, int p_fps, int p_frames,
		int p_rate = 48000, int p_mode = 0,
		Image::Format p_format = Image::FORMAT_RGBA8, bool p_channel_probe = false) {
	Owned<T> writer;
	const Ref<Image> frame = image(p_format);
	Vector<int32_t> audio = sound(p_rate / p_fps, (p_mode + 1) * 2);
	REQUIRE(frame.is_valid());
	REQUIRE_FALSE(audio.is_empty());
	if (frame.is_null() || audio.is_empty()) {
		return false;
	}
	const String path = output(p_name);
	const Error begun = writer->write_begin(Size2i(18, 18), p_fps, path);
	REQUIRE(begun == OK);
	if (begun != OK) {
		return false;
	}
	for (int i = 0; i < p_frames; i++) {
		if (p_channel_probe) {
			const int channels = (p_mode + 1) * 2;
			const int samples = p_rate / p_fps;
			const int active_channel = i / 20;
			int32_t *data = audio.ptrw();
			for (int sample = 0; sample < samples; sample++) {
				const int32_t value = int32_t(Math::sin((double(i) * samples + sample) * 6.283185307179586476925286766559 * 50.0 / p_rate) * 214748364.0);
				for (int channel = 0; channel < channels; channel++) {
					data[sample * channels + channel] = channel == active_channel ? value : 0;
				}
			}
		}
		// Distinct pictures avoid legitimate Theora duplicate-frame packets.
		frame->fill((i & 1) ? Color(1, 1, 1, 1) : Color(0, 0, 0, 1));
		const Error written = writer->write_frame(frame, audio.ptr());
		REQUIRE(written == OK);
		if (written != OK) {
			return false;
		}
	}
	writer->write_end();
	CHECK(writer->get_error() == OK);
	writer->write_end();
	CHECK(writer->get_error() == OK);
	CHECK(FileAccess::exists(path));
	const Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(path);
	CHECK_FALSE(bytes.is_empty());
	return writer->get_error() == OK && !bytes.is_empty();
}

TEST_CASE("[Movie][O02][OGV] Native zero one multi-frame and repeated "
		  "finalization retain clean ownership") {
	Settings settings;
	for (int frames : { 0, 1, 3, 16 }) {
		CHECK(
				record<Native>(vformat("native_lifecycle_%d.ogv", frames), 60, frames));
	}
	Owned<Native> writer;
	writer->write_end();
	CHECK(writer->write_begin(Size2i(17, 18), 60, output("native_odd.ogv")) ==
			ERR_INVALID_PARAMETER);
	writer->write_end();
	CHECK(writer->write_begin(Size2i(18, 18), 0, output("native_zero_fps.ogv")) ==
			ERR_INVALID_PARAMETER);
	writer->write_end();
	CHECK(writer->write_begin(Size2i(18, 18), 60,
				  output("absent-recording-directory/no.ogv")) != OK);
	writer->write_end();
	const Error begun =
			writer->write_begin(Size2i(18, 18), 60, output("native_recovered.ogv"));
	REQUIRE(begun == OK);
	if (begun != OK) {
		return;
	}
	Vector<int32_t> audio = sound(800, 2);
	CHECK(writer->write_frame(image(), audio.ptr()) == OK);
	writer->write_end();
	CHECK(writer->get_error() == OK);
	const Error geometry_begin = writer->write_begin(
			Size2i(18, 18), 60, output("native_geometry_failure.ogv"));
	REQUIRE(geometry_begin == OK);
	if (geometry_begin != OK) {
		return;
	}
	const Ref<Image> changed_size =
			Image::create_empty(20, 18, false, Image::FORMAT_RGBA8);
	REQUIRE(changed_size.is_valid());
	if (changed_size.is_null()) {
		return;
	}
	CHECK(writer->write_frame(changed_size, audio.ptr()) ==
			ERR_INVALID_PARAMETER);
	writer->write_end();
	CHECK(writer->get_error() == ERR_INVALID_PARAMETER);
	const Error restarted = writer->write_begin(
			Size2i(18, 18), 60, output("native_recovered_again.ogv"));
	REQUIRE(restarted == OK);
	if (restarted != OK) {
		return;
	}
	CHECK(writer->write_frame(image(), audio.ptr()) == OK);
	writer->write_end();
	CHECK(writer->get_error() == OK);
	writer->write_end();
	CHECK(writer->get_error() == OK);
}

TEST_CASE("[Movie][O03][OGV] Native recording retains all supplied samples at "
		  "normal high and nondivisor FPS") {
	Settings settings;
	for (int fps : { 60, 300, 59 }) {
		CHECK(record<Native>(vformat("native_fps_%d.ogv", fps), fps, 2 * fps));
	}
}

TEST_CASE("[Movie][O02][N012][N022][OGV] Native settings and normalized formats "
		  "preserve even non-macroblock dimensions") {
	Settings settings;
	for (int mode : { 0, 1, 2, 3 }) {
		settings.mode(mode);
		CHECK(record<Native>(vformat("native_mode_%d.ogv", mode), 60, 20 * ((mode + 1) * 2), 48000, mode, Image::FORMAT_RGBA8, true));
	}
	settings.mode(0);
	for (int rate : { 8000, 192000 }) {
		settings.rate(rate);
		CHECK(record<Native>(vformat("native_rate_%d.ogv", rate), 60, 3, rate));
	}
	settings.rate(48000);
	for (Image::Format format :
			{ Image::FORMAT_RGB8, Image::FORMAT_RGBA8, Image::FORMAT_L8 }) {
		CHECK(record<Native>(vformat("native_format_%d.ogv", int(format)), 60, 3,
				48000, 0, format));
	}
}

struct Failing : MovieWriter {
	Error begin_error = OK;
	Error end_error = OK;
	int begins = 0;
	int ends = 0;
	uint32_t get_audio_mix_rate() const override { return 48000; }
	AudioServer::SpeakerMode get_audio_speaker_mode() const override {
		return AudioServer::SPEAKER_MODE_STEREO;
	}
	Error write_begin(const Size2i &, uint32_t, const String &) override {
		begins++;
		return begin_error;
	}
	Error write_frame(const Ref<Image> &, const int32_t *) override {
		return ERR_FILE_CANT_WRITE;
	}
	void write_end() override {
		ends++;
		set_error(end_error);
	}
};
TEST_CASE("[Movie][O04][OGV] Common lifecycle latches begin and final errors "
		  "and finalizes once") {
	Settings settings;
	Owned<Failing> writer;
	writer->begin(Size2i(18, 18), 0, output("fake.ogv"));
	CHECK(writer->get_error() == ERR_INVALID_PARAMETER);
	CHECK(writer->begins == 0);
	writer->end();
	CHECK(writer->ends == 0);
	writer->begin_error = ERR_CANT_CREATE;
	writer->begin(Size2i(18, 18), 60, output("fake.ogv"));
	CHECK(writer->get_error() == ERR_CANT_CREATE);
	writer->end();
	CHECK(writer->ends == 0);
	writer->begin_error = OK;
	writer->end_error = ERR_FILE_CANT_WRITE;
	writer->begin(Size2i(18, 18), 60, output("fake.ogv"));
	CHECK(writer->get_error() == OK);
	writer->end();
	CHECK(writer->get_error() == ERR_FILE_CANT_WRITE);
	writer->end();
	CHECK(writer->ends == 1);
	writer->end_error = OK;
	writer->begin(Size2i(18, 18), 60, output("fake_restarted.ogv"));
	CHECK(writer->get_error() == OK);
	writer->end();
	CHECK(writer->get_error() == OK);
	CHECK(writer->ends == 2);
	writer->end();
	CHECK(writer->ends == 2);
}

#if defined(LINUXBSD_ENABLED) && defined(UNIX_ENABLED)
struct FileLimit {
	struct rlimit previous = {};
	struct sigaction previous_signal = {};
	bool active = false;
	bool set(rlim_t p_size) {
		if (getrlimit(RLIMIT_FSIZE, &previous) != 0 || p_size > previous.rlim_max) {
			return false;
		}
		struct sigaction ignore = {};
		ignore.sa_handler = SIG_IGN;
		sigemptyset(&ignore.sa_mask);
		if (sigaction(SIGXFSZ, &ignore, &previous_signal) != 0) {
			return false;
		}
		struct rlimit limited = previous;
		limited.rlim_cur = p_size;
		if (setrlimit(RLIMIT_FSIZE, &limited) != 0) {
			sigaction(SIGXFSZ, &previous_signal, nullptr);
			return false;
		}
		active = true;
		return true;
	}
	bool restore() {
		if (!active) {
			return true;
		}
		const bool restored = setrlimit(RLIMIT_FSIZE, &previous) == 0;
		const bool signal_restored =
				sigaction(SIGXFSZ, &previous_signal, nullptr) == 0;
		active = !restored;
		return restored && signal_restored;
	}
	~FileLimit() { restore(); }
};
TEST_CASE("[Movie][O04][OGV] Unix buffered final flush and close failures stay "
		  "observable after close") {
	for (bool explicit_flush : { false, true }) {
		Ref<FileAccess> file = FileAccess::open(
				output(vformat("stdio_failure_%d.bin", int(explicit_flush))),
				FileAccess::WRITE_READ);
		REQUIRE(file.is_valid());
		if (file.is_null()) {
			return;
		}
		file->seek(900);
		uint8_t data[512] = {};
		FileLimit limit;
		const bool limited = limit.set(1024);
		if (!limited) {
			CHECK(limited);
			return;
		}
		const bool stored = file->store_buffer(data, sizeof(data));
		const Error before = file->get_error();
		if (explicit_flush) {
			file->flush();
		}
		const Error after_flush = file->get_error();
		file->close();
		const Error after_close = file->get_error();
		const bool restored = limit.restore();
		CHECK(restored);
		CHECK(stored);
		CHECK(before == OK);
		if (explicit_flush) {
			CHECK(after_flush == ERR_FILE_CANT_WRITE);
		}
		CHECK(after_close == ERR_FILE_CANT_WRITE);
	}
}
TEST_CASE("[Movie][O04][OGV] Native finalization reports actual filesystem "
		  "output failure") {
	Settings settings;
	Owned<Native> writer;
	const String path = output("native_finalize_io_failure.ogv");
	const Error begun = writer->write_begin(Size2i(18, 18), 60, path);
	REQUIRE(begun == OK);
	if (begun != OK) {
		return;
	}
	const Vector<int32_t> audio = sound(800, 2);
	for (int i = 0; i < 3; i++) {
		const Error written = writer->write_frame(image(), audio.ptr());
		REQUIRE(written == OK);
		if (written != OK) {
			return;
		}
	}
	const uint64_t visible = FileAccess::get_file_as_bytes(path).size();
	FileLimit limit;
	const bool limited = limit.set(MAX(uint64_t(1), visible));
	if (!limited) {
		CHECK(limited);
		return;
	}
	writer->write_end();
	const Error result = writer->get_error();
	const bool restored = limit.restore();
	CHECK(restored);
	CHECK(result == ERR_FILE_CANT_WRITE);
	writer->write_end();
	CHECK(writer->get_error() == ERR_FILE_CANT_WRITE);
}
#endif

#ifdef MODULE_FFMPEG_ENABLED
TEST_CASE("[Movie][O01][O02][OGV] FFmpeg capability and partial initialization "
		  "remain coherent") {
	Settings settings;
	const bool available = MovieWriterOGVFFmpeg::is_available();
	print_line(vformat("O01_FFMPEG_RECORDING_AVAILABLE=%d", int(available)));
	if (!available) {
#ifdef TOOLS_ENABLED
		CHECK(Object::cast_to<MovieWriterOGV>(MovieWriter::find_writer_for_file(output("fallback.ogv"))) != nullptr);
#endif
		return;
	}
	Owned<FFmpeg> writer;
	writer->write_end();
	CHECK(writer->write_begin(Size2i(17, 18), 60, output("ffmpeg_odd.ogv")) ==
			ERR_INVALID_PARAMETER);
	writer->write_end();
	CHECK(writer->write_begin(Size2i(18, 18), 0, output("ffmpeg_zero_fps.ogv")) ==
			ERR_INVALID_PARAMETER);
	writer->write_end();
	CHECK(writer->write_begin(Size2i(18, 18), 60,
				  output("absent-recording-directory/no.ogv")) != OK);
	writer->write_end();
	const Error begun =
			writer->write_begin(Size2i(18, 18), 60, output("ffmpeg_recovered.ogv"));
	REQUIRE(begun == OK);
	if (begun != OK) {
		return;
	}
	const Vector<int32_t> audio = sound(800, 2);
	CHECK(writer->write_frame(image(), audio.ptr()) == OK);
	writer->write_end();
	CHECK(writer->get_error() == OK);
	writer->write_end();
	CHECK(writer->get_error() == OK);
	const Error geometry_begin = writer->write_begin(
			Size2i(18, 18), 60, output("ffmpeg_geometry_failure.ogv"));
	REQUIRE(geometry_begin == OK);
	if (geometry_begin != OK) {
		return;
	}
	const Ref<Image> changed_size =
			Image::create_empty(20, 18, false, Image::FORMAT_RGBA8);
	REQUIRE(changed_size.is_valid());
	if (changed_size.is_null()) {
		return;
	}
	CHECK(writer->write_frame(changed_size, audio.ptr()) ==
			ERR_INVALID_PARAMETER);
	writer->write_end();
	CHECK(writer->get_error() == ERR_INVALID_PARAMETER);
	const Error restarted = writer->write_begin(
			Size2i(18, 18), 60, output("ffmpeg_recovered_again.ogv"));
	REQUIRE(restarted == OK);
	if (restarted != OK) {
		return;
	}
	CHECK(writer->write_frame(image(), audio.ptr()) == OK);
	writer->write_end();
	CHECK(writer->get_error() == OK);
	writer->write_end();
	CHECK(writer->get_error() == OK);
}
TEST_CASE("[Movie][O03][OGV] FFmpeg recording retains actual converted samples "
		  "and final remainders") {
	Settings settings;
	if (!MovieWriterOGVFFmpeg::is_available()) {
		return;
	}
	for (int fps : { 60, 300, 59 }) {
		CHECK(record<FFmpeg>(vformat("ffmpeg_fps_%d.ogv", fps), fps, 2 * fps));
	}
	for (double quality : { -0.1, 1.0 }) {
		ProjectSettings::get_singleton()->set("editor/movie_writer/ogv/audio_quality", quality);
		CHECK(record<FFmpeg>(quality < 0 ? "ffmpeg_quality_low.ogv" : "ffmpeg_quality_high.ogv", 60, 60));
	}
}
TEST_CASE("[Movie][O02][N012][OGV] FFmpeg short recordings normalize formats "
		  "and finalize once") {
	Settings settings;
	if (!MovieWriterOGVFFmpeg::is_available()) {
		return;
	}
	for (int frames : { 0, 1, 3 }) {
		CHECK(
				record<FFmpeg>(vformat("ffmpeg_lifecycle_%d.ogv", frames), 60, frames));
	}
	for (Image::Format format :
			{ Image::FORMAT_RGB8, Image::FORMAT_RGBA8, Image::FORMAT_L8 }) {
		CHECK(record<FFmpeg>(vformat("ffmpeg_format_%d.ogv", int(format)), 60, 3,
				48000, 0, format));
	}
}
TEST_CASE("[Movie][O04][OGV] FFmpeg packet trailer and final flush errors are "
		  "latched through cleanup") {
	Settings settings;
	if (!MovieWriterOGVFFmpeg::is_available()) {
		return;
	}
	Owned<FFmpeg> writer;
	const Error begun = writer->write_begin(Size2i(18, 18), 60,
			output("ffmpeg_output_failure.ogv"));
	REQUIRE(begun == OK);
	if (begun != OK) {
		return;
	}
	const Vector<int32_t> audio = sound(800, 2);
	CHECK(writer->write_frame(image(), audio.ptr()) == OK);
	writer->test_fail_output_writes();
	writer->write_end();
	CHECK(writer->get_error() == ERR_FILE_CANT_WRITE);
	writer->write_end();
	CHECK(writer->get_error() == ERR_FILE_CANT_WRITE);
}
#endif
} // namespace TestMovieWriterOGV
#endif
