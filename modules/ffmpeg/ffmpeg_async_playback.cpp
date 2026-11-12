/**************************************************************************/
/*  ffmpeg_async_playback.cpp                                           */
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

#include "ffmpeg_async_playback.h"

#include "modules/modules_enabled.gen.h"

#ifdef MODULE_MEDIA_STREAM_ENABLED
#include "ffmpeg_adaptive_quality.h"
#include "ffmpeg_dvr.h"
#include "ffmpeg_frame_queue.h"
#include "ffmpeg_media_metadata.h"
#include "ffmpeg_quality_lookup.h"
#include "ffmpeg_stream_events.h"
#ifdef FFMPEG_TESTS_ENABLED
#include "ffmpeg_file_io.h"
#endif

#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/templates/ring_buffer.h"
#include "scene/resources/image_texture.h"
#include "servers/audio/audio_server.h"

#include "modules/media_stream/media_http.h"
#ifdef MODULE_YOUTUBE_ENABLED
#include "modules/youtube/youtube_extractor.h"
#include "modules/youtube/youtube_parser.h"
#endif

namespace {
bool valid_source(const String &p_source, bool p_allow_http = false) {
#ifdef MODULE_YOUTUBE_ENABLED
	if (!YouTubeParser::video_id(p_source).is_empty()) {
		return true;
	}
#endif
	return MediaHTTP::valid_url(p_source, p_allow_http);
}
bool resolve_source(const String &p_source, bool p_audio_only, const SafeFlag &p_cancel, String &r_url, Dictionary &r_headers, String &r_audio_url, Dictionary &r_audio_headers, String &r_message, bool *r_dvr_supported = nullptr, Dictionary *r_metadata = nullptr, int p_max_height = 1080, bool p_allow_http = false) {
	r_url = String();
	Dictionary metadata = FFmpegMediaMetadata::source(p_source);
	String url = p_source;
#ifdef MODULE_YOUTUBE_ENABLED
	if (!YouTubeParser::video_id(p_source).is_empty()) {
		YouTubeExtractor extractor(p_cancel);
		Dictionary result;
		if (extractor.resolve(p_source, p_audio_only, result, p_max_height) != OK) {
			r_message = result.get("message", "YouTube resolution failed.");
			Array diagnostics = result.get("diagnostics", Array());
			for (int i = 0; i < diagnostics.size(); i++) {
				r_message += "\n" + String(diagnostics[i]);
			}
			return false;
		}
		url = result.get("url", String());
		r_headers = result.get("http_headers", Dictionary());
		r_audio_url = result.get("audio_url", String());
		r_audio_headers = result.get("audio_http_headers", Dictionary());
		metadata["title"] = FFmpegMediaMetadata::title(result.get("title", String()));
		metadata["title_source"] = "youtube";
		metadata["source_type"] = "youtube";
		const double duration = result.get("duration", 0.0);
		metadata["duration"] = Math::is_finite(duration) ? MAX(duration, 0.0) : 0.0;
		metadata["is_live"] = result.get("is_live", false);
		if (!p_audio_only) {
			Dictionary selected = result.get("selected_format", Dictionary());
			metadata["video_width"] = selected.get("width", 0);
			metadata["video_height"] = selected.get("height", 0);
			metadata["video_fps"] = selected.get("fps", 0.0);
			metadata["video_codec"] = selected.get("vcodec", String());
			metadata["video_format_id"] = selected.get("format_id", String());
			metadata["quality_limit_height"] = p_max_height;
			metadata["adaptive_quality"] = true;
		}
		if (r_dvr_supported) {
			*r_dvr_supported = result.get("is_live_dvr_enabled", false);
		}
	}
#endif
	if (!MediaHTTP::valid_url(url, p_allow_http) || (!r_audio_url.is_empty() && !MediaHTTP::valid_url(r_audio_url, p_allow_http && url.begins_with("http://")))) {
		return false;
	}
	r_url = url;
	if (r_metadata) {
		*r_metadata = metadata;
	}
	return true;
}
bool resolve_quality(void *, const String &p_source, const SafeFlag &p_cancel, int p_limit, FFmpegQualityLookup::Result &r_result) {
	return resolve_source(p_source, false, p_cancel, r_result.video, r_result.video_headers, r_result.audio, r_result.audio_headers, r_result.message, &r_result.dvr_supported, &r_result.metadata, p_limit);
}

bool video_horizon_waiting(bool p_finite_video, double p_audio_position, double p_presentable_position, uint64_t p_without_progress_usec) {
	return p_without_progress_usec >= 100000 && p_audio_position >= p_presentable_position + (p_finite_video ? 0.05 : -0.1);
}

bool packet_source_starved(bool p_waiting, int p_queued_pcm, bool p_has_audio, bool p_has_clock, int64_t p_accepted_frames, double p_origin, double p_audible_position, int p_mix_rate) {
	if (!p_waiting || p_queued_pcm > 0) {
		return false;
	}
	if (!p_has_audio || !p_has_clock || p_mix_rate <= 0 || p_accepted_frames < 0 || !Math::is_finite(p_origin) || !Math::is_finite(p_audible_position) || p_audible_position < p_origin) {
		return true;
	}
	// Accepted real source frames include PCM in the frontend/device queues.
	// The audible clock already accounts for output latency. Allow one source
	// frame for timestamp quantization, without inventing a time-based reserve.
	return double(p_accepted_frames) - (p_audible_position - p_origin) * p_mix_rate <= 1.0;
}

#if (defined(ANDROID_ENABLED) && (defined(__arm__) || defined(__aarch64__))) || defined(FFMPEG_TESTS_ENABLED)
struct StartupFrames {
	Ref<Image> images[4];
	double positions[4] = {};
	int count = 0;
	int sampled_frames = 0;
	uint64_t serial = 0;
	uint64_t work_usec = 0;
	uint64_t update_usec = 0;
	uint64_t availability_wait_usec = 0;
	uint64_t sleep_usec = 0;
	uint64_t max_update_usec = 0;
	double media_seconds = 0.0;
	int update_calls = 0;
	int network_waits = 0;
	bool measured = false;
	bool too_slow = false;
};

// Run a bounded decoding trial before readiness. Twelve warmup frames drain
// initial codec output, then twenty-four intervals measure feeding/conversion.
// Only four diagnostic images are retained. The caller reopens after a trial;
// no trial images/PCM are played and the consumed audio clock never advances.
Error prepare_startup_frames(const Ref<VideoStreamPlayback> &p_playback, double p_fps, const SafeFlag &p_cancel,
		VideoStreamPlayback::AudioPlaybackPositionCallback p_restore_clock, void *p_restore_userdata, StartupFrames &r_frames) {
	if (p_cancel.is_set()) {
		return ERR_SKIP;
	}
	if (!Math::is_finite(p_fps) || p_fps <= 0.0 || p_fps > 60.0 || p_playback->get_channels() <= 0 ||
			!bool(p_playback->get(SNAME("_ffmpeg_async_frames"))) || (p_playback->get_length() > 0.0 && p_playback->get_length() < 1.0)) {
		return OK;
	}
	struct ClockGuard {
		Ref<VideoStreamPlayback> playback;
		VideoStreamPlayback::AudioPlaybackPositionCallback restore = nullptr;
		void *userdata = nullptr;
		double position = 0.0;
		static double read(void *p_self) { return static_cast<ClockGuard *>(p_self)->position; }
		~ClockGuard() { playback->set_audio_playback_position_callback(restore, userdata); }
	} clock{ p_playback, p_restore_clock, p_restore_userdata, p_playback->get_playback_position() };
	p_playback->set_audio_playback_position_callback(ClockGuard::read, &clock);
	const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 5000000;
	double previous_position = clock.position;
	double measured_origin = clock.position;
	constexpr int warmup_frames = 12;
	constexpr int measured_intervals = 24;
	for (int index = 0; index < warmup_frames + measured_intervals; index++) {
		const uint64_t started = OS::get_singleton()->get_ticks_usec();
		if (index > 0) {
			clock.position = previous_position + 1.0 / p_fps;
		}
		while (uint64_t(int64_t(p_playback->get(SNAME("_ffmpeg_presented_frames")))) == r_frames.serial) {
			if (p_cancel.is_set()) {
				return ERR_SKIP;
			}
			if (OS::get_singleton()->get_ticks_usec() >= deadline) {
				return ERR_TIMEOUT;
			}
			const uint64_t update_started = OS::get_singleton()->get_ticks_usec();
			p_playback->update(0.0);
			const uint64_t update_elapsed = OS::get_singleton()->get_ticks_usec() - update_started;
			if (index >= warmup_frames) {
				r_frames.update_usec += update_elapsed;
				r_frames.max_update_usec = MAX(r_frames.max_update_usec, update_elapsed);
				r_frames.update_calls++;
			}
			if (p_cancel.is_set()) {
				return ERR_SKIP;
			}
			const Error error = Error(int(p_playback->get(SNAME("stream_error"))));
			if (error != OK) {
				return error;
			}
			if (bool(p_playback->get(SNAME("_ffmpeg_packet_waiting")))) {
				if (index >= warmup_frames) {
					r_frames.network_waits++;
				}
				return OK; // Do not classify a network wait as decoder work.
			}
			if (!p_playback->is_playing()) {
				return OK; // Short tails are not CPU evidence.
			}
			if (uint64_t(int64_t(p_playback->get(SNAME("_ffmpeg_presented_frames")))) == r_frames.serial) {
				// Sparse timestamps can already have a future decoded frame.
				clock.position = MAX(clock.position, double(p_playback->get(SNAME("_ffmpeg_presentable_position"))));
				const uint64_t sleep_started = OS::get_singleton()->get_ticks_usec();
				OS::get_singleton()->delay_usec(1000);
				if (index >= warmup_frames) {
					r_frames.sleep_usec += OS::get_singleton()->get_ticks_usec() - sleep_started;
				}
			}
		}
		Ref<Image> image = p_playback->get(SNAME("_ffmpeg_cpu_frame"));
		const double frame_position = p_playback->get_playback_position();
		if (image.is_null() || image->is_empty() || image->get_width() > 1920 || image->get_height() > 1080 || !Math::is_finite(frame_position)) {
			return ERR_INVALID_DATA;
		}
		if (index < 4) {
			r_frames.images[index] = image;
			r_frames.positions[index] = frame_position;
			r_frames.count++;
		}
		previous_position = frame_position;
		r_frames.serial = int64_t(p_playback->get(SNAME("_ffmpeg_presented_frames")));
		r_frames.sampled_frames++;
		if (index == warmup_frames - 1) {
			measured_origin = frame_position;
		} else if (index >= warmup_frames) {
			r_frames.work_usec += OS::get_singleton()->get_ticks_usec() - started;
		}
	}
	if (p_cancel.is_set()) {
		return ERR_SKIP;
	}
	r_frames.media_seconds = previous_position - measured_origin;
	r_frames.availability_wait_usec = r_frames.work_usec > r_frames.update_usec + r_frames.sleep_usec ? r_frames.work_usec - r_frames.update_usec - r_frames.sleep_usec : 0;
	r_frames.measured = r_frames.media_seconds > 0.0 && r_frames.media_seconds <= 3.0;
	// Frame availability latency includes polling sleeps and time while
	// asynchronous frames become visible. Judge synchronous decode and
	// conversion cost only, preserving the existing 70% headroom threshold.
	r_frames.too_slow = r_frames.measured && r_frames.update_usec > r_frames.media_seconds * 700000.0;
	return OK;
}
#endif
} // namespace

// Decoder ownership stays on the worker. The audio callback only consumes a
// bounded PCM queue; control methods only publish commands and never do IO.
class FFmpegAsyncAudio : public AudioStreamPlaybackResampled {
	GDCLASS(FFmpegAsyncAudio, AudioStreamPlaybackResampled);
	// First member is destroyed last, after all native worker owners.
	FFmpegRetirementGuard retirement;
	Thread worker;
	SafeFlag quit;
	SafeFlag interrupt;
	mutable Mutex mutex;
	Mutex resample_mutex;
	RingBuffer<AudioFrame> pcm{ 16 };
	String path;
	Dictionary headers;
	Ref<AudioStreamMedia> owner;
	bool playing = false;
	bool finished = false;
	uint64_t generation = 0;
	double requested_position = 0;
	double position = 0;
	bool allow_http = false;
	bool prepare_requested = true;
	bool seek_requested = false;
	bool reusable_seek = false;
	FFMPEG_STREAM_EVENTS(FFmpegAsyncAudio, )

	static void run(void *p_self) { static_cast<FFmpegAsyncAudio *>(p_self)->decode(); }
	void decode() {
		Ref<AudioStreamPlayback> playback;
		String resolved_url;
		Dictionary resolved_headers = headers;
		Dictionary resolved_metadata;
		bool dvr_supported = false;
		uint64_t current = UINT64_MAX;
		AudioFrame buffer[1024];
		while (!quit.is_set()) {
			uint64_t requested;
			double from;
			double anchor;
			bool go_live;
			bool active, prepare, seeking;
			int space;
			{
				MutexLock lock(mutex);
				requested = generation;
				from = requested_position;
				anchor = dvr.anchor;
				go_live = dvr.go_live;
				active = playing && !finished;
				prepare = prepare_requested;
				seeking = seek_requested;
				space = pcm.space_left();
			}
			if (!prepare && !active) {
				if (current != requested) {
					playback.unref();
					current = requested;
				}
				OS::get_singleton()->delay_usec(2000);
				continue;
			}
			if (current != requested) {
				const bool reuse = playback.is_valid() && seeking && bool(playback->get_parameter(SNAME("_ffmpeg_reusable_seek")));
				if (!reuse) {
					playback.unref();
				}
				{
					MutexLock lock(mutex);
					if (generation != requested || quit.is_set()) {
						continue;
					}
					interrupt.clear();
				}
				if (reuse) {
					playback->set_parameter(SNAME("_ffmpeg_prepare_reuse_seek"), true);
					playback->start(from);
					if (int(playback->get_parameter(SNAME("stream_error"))) != OK) {
						playback.unref();
					}
				}
				double duration = playback.is_valid() ? owner->get_length() : 0;
				String unused_audio;
				Dictionary unused_headers;
				String message;
				Ref<FFmpegDVRWindow> window;
				if (playback.is_null() && (!resolved_url.is_empty() || resolve_source(path, true, interrupt, resolved_url, resolved_headers, unused_audio, unused_headers, message, &dvr_supported, &resolved_metadata, 1080, allow_http))) {
					publish_metadata(requested, resolved_metadata);
					if (dvr.enabled && dvr_supported) {
						window.instantiate();
						window->requested_utc = go_live || anchor < 0 ? -1 : anchor + from;
						window->requested_position = !go_live && anchor < 0 ? from : -1;
						window->required = anchor >= 0;
					}
					playback = ffmpeg_audio_playback_from_buffer(Vector<uint8_t>(), nullptr, resolved_url, Ref<AudioStream>(), resolved_headers, &interrupt, &duration, window.ptr(), true, allow_http, &message);
				}
				const bool dvr_open = window.is_valid() && bool(window->snapshot().get("available", false));
				current = requested;
				if (playback.is_valid()) {
					playback->start(dvr_open ? 0 : from);
				}
				bool failed = playback.is_null() || int(playback->get_parameter(SNAME("stream_error"))) != OK;
				Dictionary opened_metadata = failed ? Dictionary() : FFmpegMediaMetadata::opened(resolved_metadata, playback->get_parameter(SNAME("_ffmpeg_metadata")));
				{
					MutexLock lock(mutex);
					if (generation != current || quit.is_set()) {
						continue;
					}
					prepare_requested = false;
					seek_requested = false;
					events.buffering = false;
					if (failed) {
						finished = true;
						playing = false;
						events.error = ERR_CANT_OPEN;
						events.message = message.is_empty() ? "Could not open or seek the audio stream." : message;
						resolved_url = String();
					} else {
						dvr.accept(window);
						if (dvr_open) {
							position = requested_position = dvr.offset;
							duration = double(window->snapshot()["end_utc"]) - dvr.anchor;
						}
						owner->publish_remote_length(duration);
						reusable_seek = bool(playback->get_parameter(SNAME("_ffmpeg_reusable_seek")));
						set_metadata_locked(current, opened_metadata);
						events.ready = true;
					}
				}
				if (!failed) {
					post_event(current, 4);
				}
				post_event(current, failed ? 2 : 0);
				post_event(current, 1);
				if (!failed && seeking) {
					post_event(current, 3, get_playback_position());
				}
				if (failed) {
					playback.unref();
					continue;
				}
			}
			if (!active || playback.is_null() || space < 1024) {
				OS::get_singleton()->delay_usec(2000);
				continue;
			}
			int count = playback->mix(buffer, 1.0, 1024);
			bool buffered = false;
			bool error = false;
			{
				MutexLock lock(mutex);
				if (generation != current) {
					continue;
				}
				if (count > 0) {
					pcm.write(buffer, count);
					if (dvr.window.is_valid() && dvr.anchor >= 0) {
						owner->publish_remote_length(double(dvr.window->snapshot()["end_utc"]) - dvr.anchor);
					}
					buffered = events.buffering;
					events.buffering = false;
				}
				if (!playback->is_playing()) {
					finished = true;
					int code = playback->get_parameter(SNAME("stream_error"));
					if (code != OK) {
						error = true;
						events.error = ERR_CONNECTION_ERROR;
						events.message = playback->get_parameter(SNAME("_ffmpeg_error_message"));
						if (events.message.is_empty()) {
							events.message = "Audio stream reading/decoding failed.";
						}
					}
				}
			}
			if (buffered) {
				post_event(current, 1);
			}
			if (error) {
				post_event(current, 2);
			}
		}
		playback.unref();
	}

protected:
	int _mix_internal(AudioFrame *p_buffer, int p_frames) override {
		MutexLock lock(mutex);
		int count = playing ? pcm.read(p_buffer, p_frames) : 0;
		if (playing && !finished && count < p_frames && !events.buffering) {
			events.buffering = true;
			post_event(generation, 1);
		}
		for (int i = count; i < p_frames; i++) {
			p_buffer[i] = AudioFrame(0, 0);
		}
		position += double(count) / get_stream_sampling_rate();
		if (finished && pcm.data_left() == 0) {
			playing = false;
		}
		return playing ? p_frames : count;
	}
	float get_stream_sampling_rate() override { return AudioServer::get_singleton()->get_mix_rate(); }

public:
	int mix(AudioFrame *p_buffer, float p_rate_scale, int p_frames) override {
		MutexLock lock(resample_mutex);
		return AudioStreamPlaybackResampled::mix(p_buffer, p_rate_scale, p_frames);
	}
	bool initialize(const String &p_path, const Dictionary &p_headers, const Ref<AudioStreamMedia> &p_owner) {
		path = p_path;
		headers = p_headers.duplicate();
		owner = p_owner;
		dvr.enabled = owner->is_live_dvr_enabled();
		allow_http = owner->is_http_allowed();
		post_event(generation, 1);
		worker.start(run, this);
		return worker.is_started();
	}
	void start(double p_from = 0.0) override {
		seek_internal(p_from, p_from == 0);
		MutexLock lock(mutex);
		playing = true;
		finished = false;
	}
	void seek(double p_time) override { seek_internal(p_time, false); }
	void seek_internal(double p_time, bool p_go_live) {
		if (!Math::is_finite(p_time)) {
			return;
		}
		{
			MutexLock lock(mutex);
			requested_position = CLAMP(p_time, 0.0, double(INT32_MAX));
			dvr.go_live = p_go_live;
			position = requested_position;
			pcm.clear();
			finished = false;
			prepare_requested = true;
			seek_requested = true;
			events.ready = false;
			events.buffering = true;
			events.error = OK;
			generation++;
			post_event(generation, 1);
			if (!reusable_seek) {
				interrupt.set();
			}
		}
		// The callback also owns interpolation history and its read cursor.
		// Release the command mutex first: resampling calls _mix_internal().
		MutexLock resample_lock(resample_mutex);
		begin_resample();
	}
	void stop() override {
		MutexLock lock(mutex);
		playing = false;
		reusable_seek = false;
		prepare_requested = false;
		events.ready = false;
		events.buffering = false;
		pcm.clear();
		generation++;
		post_event(generation, 1);
		interrupt.set();
	}
	bool is_playing() const override {
		MutexLock lock(mutex);
		return playing;
	}
	double get_playback_position() const override {
		MutexLock lock(mutex);
		return position;
	}
	~FFmpegAsyncAudio() override {
		quit.set();
		interrupt.set();
		if (worker.is_started()) {
			worker.wait_to_finish();
		}
	}
};

class FFmpegAsyncVideo : public VideoStreamPlayback {
	GDCLASS(FFmpegAsyncVideo, VideoStreamPlayback);
	FFmpegRetirementGuard retirement;
#ifdef FFMPEG_TESTS_ENABLED
	FFmpegTestVideoFactory test_factory = nullptr;
	void *test_userdata = nullptr;
	String test_local_path;
	bool test_paired_audio = true;
	static Ref<VideoStreamPlayback> open_test_local(void *p_self) {
		FFmpegAsyncVideo *self = static_cast<FFmpegAsyncVideo *>(p_self);
		return ffmpeg_open_video_on_worker(self->test_local_path, Dictionary(), &self->interrupt, self->test_paired_audio ? self->test_local_path : String(), Dictionary());
	}
#endif
	Thread worker;
	SafeFlag quit;
	SafeFlag interrupt;
	mutable Mutex mutex;
	RingBuffer<AudioFrame> pcm{ 16 };
	String path;
	Dictionary headers;
	String audio_file;
	Dictionary audio_headers;
	FFmpegAdaptiveQuality quality;
	int quality_limit = 1080;
	bool adaptive_quality = false;
	uint64_t frontend_updates = 0;
	double frontend_timeline = 0.0;
	Ref<Texture2D> texture;
	Ref<ImageTexture> presentation_texture;
	FFmpegFrameQueue video_frames;
	bool cpu_frames = false;
	bool needs_prepared_frame = true;
	bool has_audio_track = false;
	bool audio_track_active = false;
	double displayed_position = 0.0;
	double frame_clock = 0.0;
	uint64_t frontend_uploaded_frames = 0;
	uint64_t frontend_discarded_frames = 0;
	uint64_t frontend_texture_usec = 0;
	bool playing = false;
	bool paused = false;
	uint64_t generation = 0;
	uint64_t decoding_generation = 0;
	double requested_position = 0;
	double position = 0;
	double length = 0;
	double pending_delta = 0;
	double audio_position = 0;
	bool audio_clock_available = false;
	double audio_output_latency = -1.0;
	bool live_hls = false;
	bool live_audio_waiting = false;
	bool packet_waiting = false;
#ifdef DEV_ENABLED
	bool debug_state_requested = false;
	Dictionary debug_state;
	Dictionary buffering_debug;
#endif
	double presentable_position = 0;
	double published_video_position = 0;
	bool published_video_position_valid = false;
	int64_t frontend_frames_accepted = 0;
	uint64_t presentable_progress_usec = 0;
	bool allow_http = false;
	bool prepare_requested = true;
	bool seek_requested = false;
	bool reusable_seek = false;
	uint64_t progress_usec = 0;
	FFMPEG_STREAM_EVENTS(FFmpegAsyncVideo, override)

	static void run(void *p_self) { static_cast<FFmpegAsyncVideo *>(p_self)->decode(); }
	static double read_audio_clock(void *p_self) {
		FFmpegAsyncVideo *self = static_cast<FFmpegAsyncVideo *>(p_self);
		MutexLock lock(self->mutex);
		return self->audio_clock_available ? self->audio_position - self->dvr.offset : -1.0;
	}
	// Caller holds mutex. Read the driver rate before taking that lock.
	bool source_starved_locked(int p_mix_rate) const {
		return packet_source_starved(packet_waiting, pcm.data_left(), has_audio_track, audio_clock_available, frontend_frames_accepted, requested_position, audio_position, p_mix_rate);
	}
#ifdef DEV_ENABLED
	// Caller holds mutex. Capture the assertion itself, independently of worker
	// snapshots, which cannot be collected while the worker is blocked.
	void capture_buffering_debug(const char *p_reason, int p_mix_rate) {
		const uint64_t now = OS::get_singleton()->get_ticks_usec();
		buffering_debug.clear();
		buffering_debug["reason"] = p_reason;
		buffering_debug["assertion_usec"] = int64_t(now);
		buffering_debug["generation"] = int64_t(generation);
		buffering_debug["worker_age_usec"] = int64_t(now - progress_usec);
		buffering_debug["video_age_usec"] = int64_t(now - presentable_progress_usec);
		buffering_debug["pcm_frames"] = pcm.data_left();
		buffering_debug["accepted_frames"] = frontend_frames_accepted;
		buffering_debug["mix_rate"] = p_mix_rate;
		buffering_debug["origin"] = requested_position;
		buffering_debug["audio_clock"] = audio_position;
		buffering_debug["has_audio_clock"] = audio_clock_available;
		buffering_debug["presentable"] = presentable_position;
		buffering_debug["published_position"] = published_video_position;
		buffering_debug["published_position_valid"] = published_video_position_valid;
		buffering_debug["displayed"] = displayed_position;
		buffering_debug["frame_clock"] = frame_clock;
		buffering_debug["queued_images"] = video_frames.size();
		buffering_debug["packet_waiting"] = packet_waiting;
		buffering_debug["video_waiting"] = live_audio_waiting;
	}
#endif
	static int capture(void *p_self, const float *p_samples, int p_frames) {
		FFmpegAsyncVideo *self = static_cast<FFmpegAsyncVideo *>(p_self);
		AudioFrame buffer[1024];
		MutexLock lock(self->mutex);
		if (self->generation != self->decoding_generation) {
			return 0;
		}
		int count = MIN(MIN(p_frames, 1024), self->pcm.space_left());
		for (int i = 0; i < count; i++) {
			buffer[i] = AudioFrame(p_samples[i * 2], p_samples[i * 2 + 1]);
		}
		return self->pcm.write(buffer, count);
	}
	Error capture_video_frame(const Ref<VideoStreamPlayback> &p_playback, uint64_t p_generation, double p_offset, uint64_t &r_serial) {
		const uint64_t serial = int64_t(p_playback->get(SNAME("_ffmpeg_presented_frames")));
		if (serial == r_serial) {
			return ERR_SKIP;
		}
		Ref<Image> image = p_playback->get(SNAME("_ffmpeg_cpu_frame"));
		if (image.is_null()) {
			return ERR_SKIP;
		}
		const double image_position = p_playback->get_playback_position() + p_offset;
		MutexLock lock(mutex);
		if (generation != p_generation) {
			return ERR_SKIP;
		}
		const Error result = video_frames.push(image, image_position);
		if (result == OK) {
			r_serial = serial;
			// The frontend may consume this image before the worker's batch
			// finishes. Extend the audio horizon at the same publication point;
			// a batch-end-only ledger can lag behind the displayed image.
			presentable_position = MAX(presentable_position, image_position);
			// A decoded AVFrame can advertise this PTS before conversion. Its
			// later CPU-image publication is fresh progress even when the numeric
			// horizon already includes it. Repeated/backward PTS are not progress.
			if (!published_video_position_valid || image_position > published_video_position) {
				published_video_position = image_position;
				published_video_position_valid = true;
				presentable_progress_usec = OS::get_singleton()->get_ticks_usec();
				live_audio_waiting = false;
			}
		}
		return result;
	}
	void decode() {
		Ref<VideoStreamPlayback> playback;
		String resolved_video;
		Dictionary video_headers = headers;
		String resolved_audio;
		Dictionary resolved_audio_headers = audio_headers;
		Dictionary resolved_metadata;
		bool dvr_supported = false;
		Dictionary quality_backup;
#if defined(ANDROID_ENABLED) && (defined(__arm__) || defined(__aarch64__))
		bool initial_quality_checked = false;
		Dictionary startup_metadata;
#endif
		FFmpegQualityLookup quality_lookup;
		uint64_t quality_generation = 0;
		uint64_t captured_frame_serial = 0;
		uint64_t current = UINT64_MAX;
		while (!quit.is_set()) {
			uint64_t requested;
			double from, delta, mixed_position;
			double anchor;
			bool go_live;
			bool has_audio_clock;
			bool active, is_paused, prepare, seeking, waiting_for_packets;
#ifdef DEV_ENABLED
			bool capture_debug_state;
#endif
			{
				MutexLock lock(mutex);
				requested = generation;
				from = requested_position;
				anchor = dvr.anchor;
				go_live = dvr.go_live;
				delta = pending_delta;
				mixed_position = audio_position;
				has_audio_clock = audio_clock_available;
				pending_delta = 0;
				active = playing;
				is_paused = paused;
				prepare = prepare_requested;
				seeking = seek_requested;
				waiting_for_packets = packet_waiting;
#ifdef DEV_ENABLED
				capture_debug_state = debug_state_requested;
				debug_state_requested = false;
#endif
			}
			if (quality_lookup.is_running() && (!active || is_paused)) {
				quality_lookup.cancel_and_wait();
			}
			// A same-source seek does not invalidate format selection. Let the
			// lookup finish across correction seeks, but never hand off while a
			// newer seek is still being prepared. Track selection independently
			// from the seek/event generation so corrections cannot starve lookup.
			if (requested == current && !prepare && quality_lookup.is_ready()) {
				FFmpegQualityLookup::Result next_quality;
				if (!quality_lookup.take(next_quality)) {
					continue;
				}
				MutexLock lock(mutex);
				const int selected_height = stream_metadata.get("video_height", 0);
				if (generation == current && events.ready && next_quality.generation == quality_generation && next_quality.found && playing && !paused &&
						int(next_quality.metadata.get("video_height", 0)) > 0 && int(next_quality.metadata["video_height"]) < selected_height) {
					quality_backup["video"] = resolved_video;
					quality_backup["audio"] = resolved_audio;
					quality_backup["video_headers"] = video_headers;
					quality_backup["audio_headers"] = resolved_audio_headers;
					quality_backup["metadata"] = resolved_metadata;
					quality_backup["dvr_supported"] = dvr_supported;
					quality_backup["limit"] = quality_limit;
					resolved_video = next_quality.video;
					resolved_audio = next_quality.audio;
					video_headers = next_quality.video_headers;
					resolved_audio_headers = next_quality.audio_headers;
					resolved_metadata = next_quality.metadata;
					dvr_supported = next_quality.dvr_supported;
					quality_limit = next_quality.limit;
					quality_generation++;
					// Snapshot the consumed clock after resolution, while the old
					// decoder has continued running. Only the actual handoff buffers.
					seek_locked(audio_clock_available ? audio_position : position, false);
					continue;
				}
			}
#ifdef DEV_ENABLED
			if (capture_debug_state) {
				Dictionary snapshot = playback.is_valid() ? Dictionary(playback->get(SNAME("_ffmpeg_debug_state"))) : Dictionary();
				MutexLock lock(mutex);
				snapshot["outer_playing"] = playing;
				snapshot["outer_position"] = position;
				snapshot["outer_audio_clock"] = audio_position;
				snapshot["outer_pcm_frames"] = pcm.data_left();
				snapshot["outer_presentable"] = presentable_position;
					snapshot["outer_published_position"] = published_video_position;
					snapshot["outer_published_position_valid"] = published_video_position_valid;
				snapshot["outer_live_waiting"] = live_audio_waiting;
				snapshot["outer_packet_waiting"] = packet_waiting;
				snapshot["outer_buffering"] = events.buffering;
				snapshot["outer_frontend_frames"] = frontend_frames_accepted;
				debug_state = snapshot;
				debug_state["snapshot_usec"] = int64_t(OS::get_singleton()->get_ticks_usec());
				debug_state["decoded_generation"] = int64_t(current);
				debug_state["command_generation"] = int64_t(generation);
				debug_state["frontend_updates"] = int64_t(frontend_updates);
				debug_state["frontend_uploaded_frames"] = int64_t(frontend_uploaded_frames);
				debug_state["frontend_discarded_frames"] = int64_t(frontend_discarded_frames);
				debug_state["frontend_texture_usec"] = int64_t(frontend_texture_usec);
				debug_state["frontend_position"] = displayed_position;
				debug_state["queued_video_frames"] = video_frames.size();
				debug_state["queued_video_bytes"] = video_frames.get_bytes();
			}
#endif
			if (!prepare && !active) {
				if (current != requested) {
					playback.unref();
					current = requested;
				}
				OS::get_singleton()->delay_usec(2000);
				continue;
			}
			if (current != requested) {
				const bool reuse_requested = playback.is_valid() && seeking && !bool(resolved_metadata.get("is_live", false)) && quality_backup.is_empty();
				if (!reuse_requested) {
					playback.unref();
				}
				{
					MutexLock lock(mutex);
					if (generation != requested || quit.is_set()) {
						continue;
					}
					interrupt.clear();
				}
				decoding_generation = requested;
				const bool reused = reuse_requested && ffmpeg_seek_video_on_worker(playback, from);
				if (reuse_requested && !reused) {
					playback.unref();
				}
				bool resolved = !resolved_video.is_empty();
				String message;
#ifdef FFMPEG_TESTS_ENABLED
				if (test_factory) {
					resolved = true;
					resolved_metadata = FFmpegMediaMetadata::source(path);
				}
#endif
				if (!resolved) {
					resolved = resolve_source(path, false, interrupt, resolved_video, video_headers, resolved_audio, resolved_audio_headers, message, &dvr_supported, &resolved_metadata, quality_limit, allow_http);
					if (resolved && !audio_file.is_empty()) {
						String unused_audio;
						Dictionary unused_headers;
						resolved_audio_headers = audio_headers;
						resolved = resolve_source(audio_file, true, interrupt, resolved_audio, resolved_audio_headers, unused_audio, unused_headers, message, nullptr, nullptr, 1080, allow_http && resolved_video.begins_with("http://"));
					}
				}
				Ref<FFmpegDVRWindow> window;
				if (resolved) {
					publish_metadata(requested, resolved_metadata);
					if ((dvr.enabled || adaptive_quality) && dvr_supported && audio_file.is_empty()) {
						window.instantiate();
						window->requested_utc = go_live || anchor < 0 ? -1 : anchor + from;
						window->requested_position = !go_live && anchor < 0 ? from : -1;
						window->required = anchor >= 0;
					}
#ifdef FFMPEG_TESTS_ENABLED
					if (test_factory) {
						playback = test_factory(test_userdata);
					} else
#endif
							if (!reused) {
						playback = ffmpeg_open_video_on_worker(resolved_video, video_headers, &interrupt, resolved_audio, resolved_audio_headers, &message, window.ptr(), allow_http);
					}
				}
				const bool dvr_open = window.is_valid() && bool(window->snapshot().get("available", false));
				current = requested;
				decoding_generation = current;
				bool read_ahead_ready = true;
				if (playback.is_valid()) {
					playback->set_mix_callback(capture, this);
					playback->set_audio_playback_position_callback(read_audio_clock, this);
					if (!reused) {
						playback->play();
					}
					if (!reused && from > 0 && !dvr_open) {
						playback->seek(from);
					}
					if (int(playback->get(SNAME("stream_error"))) == OK) {
#ifdef FFMPEG_TESTS_ENABLED
						if (!test_factory)
#endif
						{
							read_ahead_ready = ffmpeg_start_video_read_ahead(playback, &message, reused);
						}
					}
				}
				bool failed = !read_ahead_ready || playback.is_null() || int(playback->get(SNAME("stream_error"))) != OK || (!dvr_open && from > 0 && Math::abs(playback->get_playback_position() - from) > 0.1);
				Dictionary opened_metadata = failed ? Dictionary() : FFmpegMediaMetadata::opened(resolved_metadata, playback->get(SNAME("_ffmpeg_metadata")));
				if (!failed && adaptive_quality && (int(opened_metadata.get("video_width", 0)) > 1920 || int(opened_metadata.get("video_height", 0)) > quality_limit || double(opened_metadata.get("video_fps", 0.0)) > 60.0)) {
					failed = true;
					message = "Selected video exceeds the configured 1080p60 quality ceiling.";
				}
#if defined(ANDROID_ENABLED) && (defined(__arm__) || defined(__aarch64__))
				StartupFrames startup_frames;
				if (!failed && adaptive_quality && !initial_quality_checked) {
					const Error preparation = prepare_startup_frames(playback, opened_metadata.get("video_fps", 0.0), interrupt, read_audio_clock, this, startup_frames);
					if (preparation != OK) {
						failed = true;
						message = preparation == ERR_TIMEOUT ? "Selected video exceeds the bounded initial decoding deadline." :
								vformat("Could not prepare initial video frames (error %d).", int(preparation));
					} else if (startup_frames.too_slow) {
						failed = true;
						message = "Selected video exceeds the synchronous startup decoding budget.";
					}
					opened_metadata["startup_probe_frames"] = startup_frames.sampled_frames;
					opened_metadata["startup_probe_work_usec"] = int64_t(startup_frames.work_usec);
					opened_metadata["startup_probe_update_usec"] = int64_t(startup_frames.update_usec);
					opened_metadata["startup_probe_availability_wait_usec"] = int64_t(startup_frames.availability_wait_usec);
					opened_metadata["startup_probe_sleep_usec"] = int64_t(startup_frames.sleep_usec);
					opened_metadata["startup_probe_max_update_usec"] = int64_t(startup_frames.max_update_usec);
					opened_metadata["startup_probe_update_calls"] = startup_frames.update_calls;
					opened_metadata["startup_probe_network_waits"] = startup_frames.network_waits;
					opened_metadata["startup_probe_media_seconds"] = startup_frames.media_seconds;
					opened_metadata["startup_probe_measured"] = startup_frames.measured;
					const int fallback_height = failed ? FFmpegAdaptiveQuality::lower_height(int(resolved_metadata.get("video_height", 0))) : 0;
					const int playback_error = playback.is_valid() ? int(playback->get(SNAME("stream_error"))) : int(ERR_UNCONFIGURED);
					print_line(vformat("FFmpeg: Android startup quality probe budget_basis=synchronous_update_70pct format=%s selected=%dx%d@%.3ffps codec=%s frames=%d media=%.3fs elapsed=%dus update=%dus availability_wait=%dus sleep=%dus update_calls=%d max_update=%dus network_waits=%d measured=%s too_slow=%s preparation_error=%d playback_error=%d fallback_height=%d reason=%s",
							String(resolved_metadata.get("video_format_id", String("unknown"))), int(opened_metadata.get("video_width", 0)), int(opened_metadata.get("video_height", 0)), double(opened_metadata.get("video_fps", 0.0)),
							String(opened_metadata.get("video_codec", resolved_metadata.get("video_codec", String("unknown")))), startup_frames.sampled_frames, startup_frames.media_seconds,
							int64_t(startup_frames.work_usec), int64_t(startup_frames.update_usec), int64_t(startup_frames.availability_wait_usec), int64_t(startup_frames.sleep_usec), startup_frames.update_calls,
							int64_t(startup_frames.max_update_usec), startup_frames.network_waits, startup_frames.measured, startup_frames.too_slow, int(preparation), playback_error, fallback_height, message));
					if (!failed && startup_frames.sampled_frames > 0 && !interrupt.is_set()) {
						// A bounded trial can decode ahead of the initial image. Reopen
						// the same validated selection before readiness; no trial PCM or
						// images are played, and the source begins at its proper origin.
						startup_metadata["startup_probe_frames"] = startup_frames.sampled_frames;
						startup_metadata["startup_probe_work_usec"] = int64_t(startup_frames.work_usec);
						startup_metadata["startup_probe_update_usec"] = int64_t(startup_frames.update_usec);
						startup_metadata["startup_probe_availability_wait_usec"] = int64_t(startup_frames.availability_wait_usec);
						startup_metadata["startup_probe_sleep_usec"] = int64_t(startup_frames.sleep_usec);
						startup_metadata["startup_probe_max_update_usec"] = int64_t(startup_frames.max_update_usec);
						startup_metadata["startup_probe_update_calls"] = startup_frames.update_calls;
						startup_metadata["startup_probe_network_waits"] = startup_frames.network_waits;
						startup_metadata["startup_probe_media_seconds"] = startup_frames.media_seconds;
						startup_metadata["startup_probe_measured"] = startup_frames.measured;
						initial_quality_checked = true;
						playback.unref();
						{
							MutexLock lock(mutex);
							pcm.clear();
							video_frames.clear();
						}
						current = UINT64_MAX;
						continue;
					}
				}
				if (initial_quality_checked) {
					opened_metadata.merge(startup_metadata, true);
				}
#endif
				if (failed && adaptive_quality && quality_backup.is_empty() && !interrupt.is_set() && (message.begins_with("Timed out buffering remote") || message.begins_with("Selected video exceeds"))) {
					const int lower = FFmpegAdaptiveQuality::lower_height(int(resolved_metadata.get("video_height", 0)));
					if (lower > 0) {
#if defined(ANDROID_ENABLED) && (defined(__arm__) || defined(__aarch64__))
						if (!startup_frames.update_calls) {
							print_line(vformat("FFmpeg: Android startup quality rung selected=%dx%d@%.3ffps codec=%s format=%s probe=skipped fallback_height=%d reason=%s",
									int(resolved_metadata.get("video_width", 0)), int(resolved_metadata.get("video_height", 0)), double(resolved_metadata.get("video_fps", 0.0)),
									String(resolved_metadata.get("video_codec", String("unknown"))), String(resolved_metadata.get("video_format_id", String("unknown"))), lower, message));
						}
#endif
#if defined(ANDROID_ENABLED) && (defined(__arm__) || defined(__aarch64__))
						initial_quality_checked = false;
						startup_metadata.clear();
#endif
						playback.unref();
						{
							MutexLock lock(mutex);
							pcm.clear();
							video_frames.clear();
						}
						resolved_video = String();
						resolved_audio = String();
						quality_limit = lower;
						current = UINT64_MAX;
						continue;
					}
				}
				if (failed && message.is_empty() && playback.is_valid()) {
					message = vformat("Could not start or seek the paired media at %.3f seconds (error %d; prepared %.3f seconds).", from, int(playback->get(SNAME("stream_error"))), playback->get_playback_position());
					const String detail = playback->get(SNAME("_ffmpeg_error_message"));
					if (!detail.is_empty()) {
						message += " " + detail;
					}
				}
				if (failed && !quality_backup.is_empty() && !interrupt.is_set()) {
					// The lower rung could disappear or fail to open. Restore the
					// last working selection once, at the same requested position.
					playback.unref();
					resolved_video = quality_backup["video"];
					resolved_audio = quality_backup["audio"];
					video_headers = quality_backup["video_headers"];
					resolved_audio_headers = quality_backup["audio_headers"];
					resolved_metadata = quality_backup["metadata"];
					dvr_supported = quality_backup["dvr_supported"];
					quality_limit = quality_backup["limit"];
					quality_backup.clear();
					quality.disable();
					current = UINT64_MAX;
					continue;
				}
				{
					MutexLock lock(mutex);
					if (generation != current || quit.is_set()) {
						continue;
					}
					prepare_requested = false;
					seek_requested = false;
					events.buffering = false;
					if (failed) {
						playing = false;
						events.error = ERR_CANT_OPEN;
						events.message = message.is_empty() ? "Could not open or seek the paired video/audio streams." : message;
						resolved_video = String();
						resolved_audio = String();
					} else {
						if (!dvr.enabled && dvr.anchor < 0 && dvr_open) {
							// Keep a private UTC handoff anchor for automatic live
							// quality changes without advertising opt-in DVR controls.
							dvr.anchor = window->snapshot()["selected_utc"];
						}
						dvr.accept(window);
						if (dvr_open) {
							position = requested_position = dvr.offset;
							audio_position = position;
							mixed_position = position;
							frontend_frames_accepted = 0;
						}
						set_metadata_locked(current, opened_metadata);
						events.ready = true;
						// Opening and decoder preparation are not playback time. Audio
						// starts at `from`, so video must not catch up to time spent here.
						pending_delta = 0;
						delta = 0;
						length = playback->get_length();
						reusable_seek = ffmpeg_can_reuse_video_seek(playback);
						cpu_frames = bool(playback->get(SNAME("_ffmpeg_async_frames")));
						has_audio_track = playback->get_channels() > 0;
						audio_track_active = has_audio_track;
						captured_frame_serial = 0;
						published_video_position_valid = false;
						needs_prepared_frame = true;
						// Live opening and seek preroll can already have a CPU image.
						// Queue it before readiness is observable on the frontend.
						if (cpu_frames) {
							Ref<Image> prepared = playback->get(SNAME("_ffmpeg_cpu_frame"));
							if (prepared.is_valid() && video_frames.size() == 0) {
								const double prepared_position = playback->get_playback_position() + dvr.offset;
								const Error queued = video_frames.push(prepared, prepared_position);
								if (queued == OK) {
									captured_frame_serial = int64_t(playback->get(SNAME("_ffmpeg_presented_frames")));
									published_video_position = prepared_position;
									published_video_position_valid = true;
								} else {
									failed = true;
									playing = false;
									events.ready = false;
									events.error = ERR_CANT_OPEN;
									events.message = "Could not queue the prepared video frame.";
									pcm.clear();
								}
							}
						}
						displayed_position = frame_clock = position;
						texture = cpu_frames ? Ref<Texture2D>(presentation_texture) : playback->get_texture();
						live_hls = bool(playback->get(SNAME("_ffmpeg_live_hls")));
						presentable_position = double(playback->get(SNAME("_ffmpeg_presentable_position"))) + dvr.offset;
						if (published_video_position_valid) {
							presentable_position = MAX(presentable_position, published_video_position);
						}
						presentable_progress_usec = OS::get_singleton()->get_ticks_usec();
						progress_usec = OS::get_singleton()->get_ticks_usec();
						quality.opened(progress_usec);
						quality_backup.clear();
#if defined(ANDROID_ENABLED) && (defined(__arm__) || defined(__aarch64__))
						initial_quality_checked = !failed;
#endif
					}
				}
				if (!failed) {
					post_event(current, 4);
				}
				post_event(current, failed ? 2 : 0);
				post_event(current, 1);
				if (!failed && seeking) {
					post_event(current, 3, get_playback_position());
				}
				if (failed) {
					playback.unref();
					continue;
				}
			}
			if (!active || playback.is_null()) {
				OS::get_singleton()->delay_usec(2000);
				continue;
			}
			playback->set_paused(is_paused);
			const double timeline_offset = dvr.offset;
			mixed_position -= timeline_offset;
			if (has_audio_clock) {
				playback->set_audio_playback_position(mixed_position);
			}
			bool frame_error = false;
			if (cpu_frames && !is_paused) {
				for (int batch = 0; batch < 5; batch++) {
					const Error queued = capture_video_frame(playback, current, timeline_offset, captured_frame_serial);
					if (queued != OK && queued != ERR_SKIP && queued != ERR_BUSY) {
						frame_error = true;
						break;
					}
					bool room;
					{
						MutexLock lock(mutex);
						room = generation == current && video_frames.has_room() && queued != ERR_BUSY;
					}
					playback->set(SNAME("_ffmpeg_frame_queue_full"), !room);
					const uint64_t before = int64_t(playback->get(SNAME("_ffmpeg_presented_frames")));
					playback->update(batch == 0 ? delta : 0.0);
					if (!room || before == uint64_t(int64_t(playback->get(SNAME("_ffmpeg_presented_frames"))))) {
						break;
					}
				}
			} else if (!is_paused && (delta > 0 || waiting_for_packets || (has_audio_clock && mixed_position > playback->get_playback_position()))) {
				playback->update(delta);
			}
			bool buffered = false;
			bool failed = frame_error;
			bool ended = frame_error;
			const int source_mix_rate = get_mix_rate();
			{
				MutexLock lock(mutex);
				if (generation == current) {
					packet_waiting = bool(playback->get(SNAME("_ffmpeg_packet_waiting")));
					const double next_presentable = double(playback->get(SNAME("_ffmpeg_presentable_position"))) + timeline_offset;
					if (Math::is_finite(next_presentable) && next_presentable > presentable_position) {
						presentable_position = next_presentable;
						presentable_progress_usec = OS::get_singleton()->get_ticks_usec();
						live_audio_waiting = false;
					}
					// Pause may have arrived while decode waited for network input.
					// Preserve the paused public snapshot instead of committing that
					// in-flight update after set_paused() has already returned.
					if (!paused) {
						position = playback->get_playback_position() + timeline_offset;
						if (!cpu_frames) {
							texture = playback->get_texture();
						}
					}
					audio_track_active = bool(playback->get(SNAME("_ffmpeg_audio_active")));
					length = playback->get_length();
					progress_usec = OS::get_singleton()->get_ticks_usec();
					const bool source_starved = source_starved_locked(source_mix_rate);
					buffered = events.buffering && !live_audio_waiting && !source_starved;
					if (!live_audio_waiting && !source_starved) {
						events.buffering = false;
					}
					if (frame_error) {
						playing = false;
						events.error = ERR_INVALID_DATA;
						events.message = "Decoded video frame exceeds the bounded presentation queue.";
						pcm.clear();
						video_frames.clear();
					} else if (!playback->is_playing()) {
						ended = true;
						playing = false;
						if (int(playback->get(SNAME("stream_error"))) != OK) {
							failed = true;
							const int code = int(playback->get(SNAME("stream_error")));
							events.error = code == ERR_UNAVAILABLE ? ERR_UNAVAILABLE : ERR_CONNECTION_ERROR;
							const String detail = playback->get(SNAME("_ffmpeg_error_message"));
							events.message = detail.is_empty() ? "Video stream reading/decoding failed." : detail;
							pcm.clear();
							video_frames.clear();
						}
						buffered = buffered || events.buffering;
						live_audio_waiting = false;
						packet_waiting = false;
						events.buffering = false;
					}
				}
			}
			if (buffered) {
				post_event(current, 1);
			}
			if (failed) {
				post_event(current, 1);
				post_event(current, 2);
			}
			if (ended) {
				// EOF no longer needs demuxers or packet producers. The frontend
				// can drain the retained PCM independently of input ownership.
				playback.unref();
			}
			if (adaptive_quality && playback.is_valid()) {
				bool suspended;
				bool starved;
				int selected_height;
				double selected_fps;
				uint64_t observed_frontend;
				uint64_t observed_uploads;
				double expected_clock;
				const double audio_backlog = double(playback->get(SNAME("_ffmpeg_audio_backlog")));
				{
					MutexLock lock(mutex);
					suspended = generation != current || paused || !playing || !events.ready;
					// A live quality switch must reopen the same advertised UTC
					// position. Without that timeline, retain the working decoder
					// rather than seeking a nonseekable rendition after closing it.
					suspended = suspended || (live_hls && dvr.anchor < 0);
					starved = pcm.data_left() < 1024 && (packet_waiting || audio_backlog > 0.25);
					// Finite A/V backpressure can fill the PCM queue while video
					// falls behind requested time. That is decoder pressure too.
					starved = starved || (!live_hls && has_audio_track && audio_backlog > 0.25);
					selected_height = stream_metadata.get("video_height", 0);
					selected_fps = stream_metadata.get("video_fps", 0.0);
					observed_frontend = frontend_updates;
					observed_uploads = frontend_uploaded_frames;
					expected_clock = frontend_timeline;
				}
				const bool late = double(playback->get(SNAME("_ffmpeg_video_lateness"))) > 0.25;
				const uint64_t presented_frames = cpu_frames ? observed_uploads : uint64_t(int64_t(playback->get(SNAME("_ffmpeg_presented_frames"))));
				const int lower = quality.observe(OS::get_singleton()->get_ticks_usec(), selected_height, late || starved, suspended, presented_frames, mixed_position, selected_fps, observed_frontend, expected_clock);
				if (lower > 0 && !quality_lookup.is_running()) {
					// URL extraction can take seconds. Keep decoding this selection
					// until one bounded background lookup supplies a valid replacement.
					quality_lookup.start(path, quality_generation, lower, resolve_quality);
				}
			}
			OS::get_singleton()->delay_usec(2000);
		}
		quality_lookup.cancel_and_wait();
		playback.unref();
	}

public:
#ifdef FFMPEG_TESTS_ENABLED
	void set_test_factory(FFmpegTestVideoFactory p_factory, void *p_userdata) {
		test_factory = p_factory;
		test_userdata = p_userdata;
	}
	bool initialize_test_local(const String &p_path, bool p_paired_audio) {
		ERR_FAIL_COND_V(p_path.is_empty() || FFmpegFileIO::is_remote(p_path), false);
		test_local_path = p_path;
		test_paired_audio = p_paired_audio;
		set_test_factory(open_test_local, this);
		return initialize(p_path, Dictionary(), String(), Dictionary(), false, false);
	}
#endif
#ifdef DEV_ENABLED
	bool _set(const StringName &p_name, const Variant &p_value) {
		if (p_name == SNAME("_ffmpeg_debug_state_requested")) {
			MutexLock lock(mutex);
			debug_state_requested = bool(p_value);
			debug_state.clear();
			return true;
		}
		return false;
	}
#endif
	bool _get(const StringName &p_name, Variant &r_result) const {
		if (p_name == SNAME("_ffmpeg_generation")) {
			MutexLock lock(mutex);
			r_result = int64_t(generation);
			return true;
		}
#ifdef DEV_ENABLED
		if (p_name == SNAME("_ffmpeg_debug_state")) {
			MutexLock lock(mutex);
			r_result = debug_state.duplicate();
			return true;
		}
		if (p_name == SNAME("_ffmpeg_buffering_debug")) {
			MutexLock lock(mutex);
			r_result = buffering_debug.duplicate();
			return true;
		}
#endif
		return false;
	}
	bool initialize(const String &p_path, const Dictionary &p_headers, const String &p_audio_file, const Dictionary &p_audio_headers, bool p_live_dvr_enabled, bool p_allow_http) {
		presentation_texture.instantiate();
		texture = presentation_texture;
		path = p_path;
		headers = p_headers.duplicate();
		audio_file = p_audio_file;
		audio_headers = p_audio_headers.duplicate();
		dvr.enabled = p_live_dvr_enabled;
		allow_http = p_allow_http;
#ifdef MODULE_YOUTUBE_ENABLED
		adaptive_quality = audio_file.is_empty() && !YouTubeParser::video_id(path).is_empty();
#endif
		post_event(generation, 1);
		worker.start(run, this);
		return worker.is_started();
	}
	void update(double p_delta) override {
		bool limit_video_audio = false;
		Ref<FFmpegAsyncVideo> keep_alive(this);
		const int mix_rate = get_mix_rate();
		double output_latency = -1.0;
		Ref<Image> next_image;
		double next_image_position = 0.0;
		uint64_t image_generation = 0;
		int discarded_images = 0;
		bool prepared_image = false;
		{
			MutexLock lock(mutex);
			prepared_image = needs_prepared_frame && video_frames.size() > 0;
			if (!events.ready || (paused && !prepared_image)) {
				return;
			}
			limit_video_audio = audio_clock_available && (live_hls || (cpu_frames && has_audio_track && length > 0.0));
			frontend_updates++;
			if (events.ready) {
				frontend_timeline += paused ? 0.0 : MAX(p_delta, 0.0);
				if (cpu_frames) {
					if (paused) {
						// A prepared poster may change while paused; media time cannot.
					} else if (!has_audio_track || (!audio_track_active && pcm.data_left() == 0 && audio_position <= frame_clock)) {
						frame_clock += MAX(p_delta, 0.0);
					} else {
						frame_clock = MAX(frame_clock, audio_position);
					}
					const double fps = stream_metadata.get("video_fps", 0.0);
					next_image = prepared_image ? video_frames.take_first(next_image_position) : video_frames.take_due(frame_clock, next_image_position, discarded_images, fps > 0.0 ? 1.0 / fps : 0.0);
					if (next_image.is_valid()) {
						needs_prepared_frame = false;
					}
					image_generation = generation;
				}
			}
			output_latency = audio_output_latency;
			if (playing && !paused) {
				if (events.ready && !events.buffering) {
					pending_delta += MAX(p_delta, 0.0);
				}
				if (events.ready && !events.buffering && OS::get_singleton()->get_ticks_usec() - progress_usec > 250000) {
#ifdef DEV_ENABLED
					capture_buffering_debug("worker_timeout", mix_rate);
#endif
					events.buffering = true;
					post_event(generation, 1);
				}
				if (events.ready && source_starved_locked(mix_rate) && !events.buffering) {
#ifdef DEV_ENABLED
					capture_buffering_debug("source_starved", mix_rate);
#endif
					events.buffering = true;
					post_event(generation, 1);
				}
			}
		}
		if (next_image.is_valid()) {
			{
				MutexLock lock(mutex);
				if (generation != image_generation || (paused && !prepared_image)) {
					return;
				}
				// Resource notifications can reenter pause/seek/stream replacement.
				// Their snapshot must already identify the frame being published.
				displayed_position = next_image_position;
			}
			// This frontend method is driven by the player's main frame tick.
			// Conversion and network work have already finished on the worker.
			const uint64_t upload_started = OS::get_singleton()->get_ticks_usec();
			if (presentation_texture->get_width() == next_image->get_width() && presentation_texture->get_height() == next_image->get_height()) {
				presentation_texture->update(next_image);
			} else {
				presentation_texture->set_image(next_image);
			}
			MutexLock lock(mutex);
			if (generation == image_generation) {
				frontend_uploaded_frames++;
				frontend_discarded_frames += discarded_images;
				frontend_texture_usec += OS::get_singleton()->get_ticks_usec() - upload_started;
			}
		}
		{
			MutexLock lock(mutex);
			if (paused || !events.ready) {
				return;
			}
		}
		if (output_latency < 0.0) {
			output_latency = limit_video_audio ? AudioServer::get_singleton()->get_output_latency() : 0.0;
		}
		if (!Math::is_finite(output_latency) || output_latency < 0.0) {
			output_latency = 0.0;
		}
		AudioFrame samples[1024];
		float interleaved[2048];
		for (int batch = 0; batch < 16; batch++) {
			int count;
			uint64_t current;
			{
				MutexLock lock(mutex);
				current = generation;
				count = pcm.read(samples, 1024, false);
				const bool finite_video = cpu_frames && has_audio_track && length > 0.0 && !live_hls;
				if (audio_clock_available && (live_hls || (finite_video && playing))) {
					// Samples accepted by the frontend include device output latency.
					// Keep the audible clock within 100ms of a decoded/queued frame,
					// holding PCM during slow video fetching or conversion. Finite
					// sources release this bound at EOF so their audio tail drains.
					const double allowance_seconds = CLAMP(presentable_position - requested_position + 0.1 + output_latency, 0.0, double(INT32_MAX));
					const int64_t budget = MAX(int64_t(0), int64_t(allowance_seconds * mix_rate) - frontend_frames_accepted);
					count = int(MIN(int64_t(count), budget));
					if (!playing && budget == 0) {
						// No future video frame can extend this terminal live horizon.
						// Keep partial frontend acceptance until the playable tail is
						// delivered, then discard only PCM beyond that final horizon.
						pcm.clear();
					}
					// Hold bounded PCM immediately, but a normal frame handoff is
					// not a buffering transition. Both finite and live sources must
					// have stopped extending their video horizon for at least 100 ms.
					const bool video_wait = video_horizon_waiting(finite_video, audio_position, presentable_position, OS::get_singleton()->get_ticks_usec() - presentable_progress_usec);
					if (budget == 0 && events.ready && playing && video_wait && !live_audio_waiting) {
						live_audio_waiting = true;
						if (!events.buffering) {
#ifdef DEV_ENABLED
							capture_buffering_debug("video_horizon", mix_rate);
#endif
							events.buffering = true;
							post_event(current, 1);
						}
					}
				}
			}
			if (count == 0 || !mix_callback) {
				break;
			}
			for (int i = 0; i < count; i++) {
				interleaved[i * 2] = samples[i].left;
				interleaved[i * 2 + 1] = samples[i].right;
			}
			int accepted = CLAMP(mix_callback(mix_udata, interleaved, count), 0, count);
			{
				MutexLock lock(mutex);
				if (generation != current) {
					break;
				}
				pcm.advance_read(accepted);
				frontend_frames_accepted += accepted;
			}
			if (accepted < count) {
				break;
			}
		}
	}

	void play() override {
		seek_internal(0, true);
		MutexLock lock(mutex);
		playing = true;
	}
	void stop() override {
		seek(0);
		MutexLock lock(mutex);
		playing = false;
		reusable_seek = false;
		prepare_requested = false;
		interrupt.set();
		events.buffering = false;
		post_event(generation, 1);
	}
	void seek(double p_time) override { seek_internal(p_time, false); }
	void seek_internal(double p_time, bool p_go_live) {
		if (!Math::is_finite(p_time)) {
			return;
		}
		MutexLock lock(mutex);
		seek_locked(p_time, p_go_live);
	}
	// Caller holds the command mutex. Used by public seeks and worker handoffs.
	void seek_locked(double p_time, bool p_go_live) {
		if (p_go_live && !dvr.enabled && adaptive_quality) {
			dvr.anchor = -1;
			dvr.window.unref();
		}
		requested_position = CLAMP(p_time, 0.0, double(INT32_MAX));
		dvr.go_live = p_go_live;
		position = requested_position;
		displayed_position = frame_clock = requested_position;
		video_frames.clear();
		published_video_position = requested_position;
		published_video_position_valid = false;
		needs_prepared_frame = true;
		audio_position = requested_position;
		live_hls = false;
		live_audio_waiting = false;
		packet_waiting = false;
		presentable_position = requested_position;
		frontend_frames_accepted = 0;
		presentable_progress_usec = 0;
		pending_delta = 0;
		pcm.clear();
		prepare_requested = true;
		seek_requested = true;
		events.ready = false;
		events.buffering = true;
		events.error = OK;
		generation++;
		post_event(generation, 1);
		if (!reusable_seek) {
			interrupt.set();
		}
	}
	bool is_playing() const override {
		MutexLock lock(mutex);
		return playing || pcm.data_left() > 0 || video_frames.size() > 0;
	}
	void set_paused(bool p_paused) override {
		MutexLock lock(mutex);
		paused = p_paused;
	}
	bool is_paused() const override {
		MutexLock lock(mutex);
		return paused;
	}
	double get_length() const override {
		MutexLock lock(mutex);
		if (adaptive_quality && !dvr.enabled && bool(stream_metadata.get("is_live", false))) {
			return 0.0;
		}
		return dvr.enabled && dvr.window.is_valid() && dvr.anchor >= 0 ? double(dvr.window->snapshot()["end_utc"]) - dvr.anchor : length;
	}
	double get_playback_position() const override {
		MutexLock lock(mutex);
		return cpu_frames ? displayed_position : position;
	}
	Ref<Texture2D> get_texture() const override {
		MutexLock lock(mutex);
		return texture;
	}
	int get_channels() const override { return 2; }
	int get_mix_rate() const override { return int(AudioServer::get_singleton()->get_mix_rate()); }
	void set_audio_playback_position(double p_position) override {
		MutexLock lock(mutex);
		audio_position = p_position;
		audio_clock_available = true;
	}
	void set_audio_output_latency(double p_source_seconds) override {
		if (!Math::is_finite(p_source_seconds) || p_source_seconds < 0.0) {
			return;
		}
		MutexLock lock(mutex);
		audio_output_latency = p_source_seconds;
	}
	~FFmpegAsyncVideo() override {
		quit.set();
		interrupt.set();
		if (worker.is_started()) {
			worker.wait_to_finish();
		}
	}
};

Ref<AudioStreamPlayback> ffmpeg_async_audio(const String &p_path, const Dictionary &p_headers, const Ref<AudioStreamMedia> &p_owner) {
	if (!valid_source(p_path, p_owner->is_http_allowed())) {
		return Ref<AudioStreamPlayback>();
	}
	Ref<FFmpegAsyncAudio> playback;
	playback.instantiate();
	return playback->initialize(p_path, p_headers, p_owner) ? Ref<AudioStreamPlayback>(playback) : Ref<AudioStreamPlayback>();
}

Ref<VideoStreamPlayback> ffmpeg_async_video(const String &p_path, const Dictionary &p_headers, const String &p_audio_file, const Dictionary &p_audio_headers, bool p_live_dvr_enabled, bool p_allow_http) {
	if (!valid_source(p_path, p_allow_http) || (!p_audio_file.is_empty() && !valid_source(p_audio_file, p_allow_http && p_path.begins_with("http://")))) {
		return Ref<VideoStreamPlayback>();
	}
	Ref<FFmpegAsyncVideo> playback;
	playback.instantiate();
	return playback->initialize(p_path, p_headers, p_audio_file, p_audio_headers, p_live_dvr_enabled, p_allow_http) ? Ref<VideoStreamPlayback>(playback) : Ref<VideoStreamPlayback>();
}
#ifdef FFMPEG_TESTS_ENABLED
class FFmpegTestVideoStream : public VideoStream {
	GDCLASS(FFmpegTestVideoStream, VideoStream);
	bool paired_audio = true;

protected:
	static void _bind_methods() {
		ClassDB::bind_method(D_METHOD("set_paired_audio", "enabled"), &FFmpegTestVideoStream::set_paired_audio);
		ClassDB::bind_method(D_METHOD("is_paired_audio"), &FFmpegTestVideoStream::is_paired_audio);
		ADD_PROPERTY(PropertyInfo(Variant::BOOL, "paired_audio"), "set_paired_audio", "is_paired_audio");
	}

public:
	void set_paired_audio(bool p_enabled) { paired_audio = p_enabled; }
	bool is_paired_audio() const { return paired_audio; }
	Ref<VideoStreamPlayback> instantiate_playback() override {
		Ref<FFmpegAsyncVideo> playback;
		playback.instantiate();
		return playback->initialize_test_local(get_file(), paired_audio) ? Ref<VideoStreamPlayback>(playback) : Ref<VideoStreamPlayback>();
	}
};
#endif

void register_ffmpeg_async_playback_types() {
	GDREGISTER_INTERNAL_CLASS(FFmpegRetirementFence);
	GDREGISTER_INTERNAL_CLASS(FFmpegAsyncAudio);
	GDREGISTER_INTERNAL_CLASS(FFmpegAsyncVideo);
#ifdef FFMPEG_TESTS_ENABLED
	GDREGISTER_CLASS(FFmpegTestVideoStream);
#endif
}
#ifdef FFMPEG_TESTS_ENABLED
bool ffmpeg_test_packet_source_starved(bool p_waiting, int p_queued_pcm, int64_t p_accepted_frames, double p_origin, double p_audible_position, int p_mix_rate, bool p_has_clock, bool p_has_audio) {
	return packet_source_starved(p_waiting, p_queued_pcm, p_has_audio, p_has_clock, p_accepted_frames, p_origin, p_audible_position, p_mix_rate);
}

bool ffmpeg_test_video_horizon_waiting(bool p_finite_video, double p_audio_position, double p_presentable_position, uint64_t p_without_progress_usec) {
	return video_horizon_waiting(p_finite_video, p_audio_position, p_presentable_position, p_without_progress_usec);
}

Dictionary ffmpeg_test_startup_frames(const Ref<VideoStreamPlayback> &p_playback, double p_fps, const SafeFlag &p_cancel,
		VideoStreamPlayback::AudioPlaybackPositionCallback p_restore_clock, void *p_restore_userdata) {
	StartupFrames frames;
	const Error error = prepare_startup_frames(p_playback, p_fps, p_cancel, p_restore_clock, p_restore_userdata, frames);
	Dictionary result;
	result["error"] = int(error);
	result["measured"] = frames.measured;
	result["too_slow"] = frames.too_slow;
	result["work_usec"] = int64_t(frames.work_usec);
	result["update_usec"] = int64_t(frames.update_usec);
	result["availability_wait_usec"] = int64_t(frames.availability_wait_usec);
	result["sleep_usec"] = int64_t(frames.sleep_usec);
	result["max_update_usec"] = int64_t(frames.max_update_usec);
	result["update_calls"] = frames.update_calls;
	result["network_waits"] = frames.network_waits;
	result["sampled_frames"] = frames.sampled_frames;
	result["media_seconds"] = frames.media_seconds;
	Array images;
	Array positions;
	for (int index = 0; index < frames.count; index++) {
		images.append(frames.images[index]);
		positions.append(frames.positions[index]);
	}
	result["images"] = images;
	result["positions"] = positions;
	return result;
}

Ref<VideoStreamPlayback> ffmpeg_test_async_video(FFmpegTestVideoFactory p_factory, void *p_userdata) {
	ERR_FAIL_NULL_V(p_factory, Ref<VideoStreamPlayback>());
	Ref<FFmpegAsyncVideo> playback;
	playback.instantiate();
	playback->set_test_factory(p_factory, p_userdata);
	return playback->initialize("test://finite-video", Dictionary(), String(), Dictionary(), false, false) ? Ref<VideoStreamPlayback>(playback) : Ref<VideoStreamPlayback>();
}
#endif
#endif // MODULE_MEDIA_STREAM_ENABLED
