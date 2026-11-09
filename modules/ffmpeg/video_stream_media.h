#ifndef VIDEO_STREAM_MEDIA_H
#define VIDEO_STREAM_MEDIA_H

#include "core/io/resource_loader.h"
#include "core/templates/safe_refcount.h"
#include "scene/resources/video_stream.h"
#include "servers/audio/audio_stream.h"

class OggPacketSequence;

class VideoStreamPlaybackFFmpeg;
class FFmpegDVRWindow;

struct FFmpegAudioMetadata {
	int channels = 0;
	int sample_rate = 0;
	double length = 0.0;
};

class VideoStreamMedia : public VideoStream {
	GDCLASS(VideoStreamMedia, VideoStream);
	Dictionary http_headers;
	String audio_file;
	Dictionary audio_http_headers;
	bool live_dvr_enabled = false;
	bool allow_http = false;

protected:
	static void _bind_methods();

public:
	void set_live_dvr_enabled(bool p_enabled) { live_dvr_enabled = p_enabled; }
	bool is_live_dvr_enabled() const { return live_dvr_enabled; }
	void set_allow_http(bool p_enabled) { allow_http = p_enabled; }
	bool is_http_allowed() const { return allow_http; }
	void set_http_headers(const Dictionary &p_headers) { http_headers = p_headers.duplicate(); }
	Dictionary get_http_headers() const { return http_headers.duplicate(); }
	void set_audio_file(const String &p_file) { audio_file = p_file; }
	String get_audio_file() const { return audio_file; }
	void set_audio_http_headers(const Dictionary &p_headers) { audio_http_headers = p_headers.duplicate(); }
	Dictionary get_audio_http_headers() const { return audio_http_headers.duplicate(); }
	Ref<VideoStreamPlayback> instantiate_playback() override;
};

class AudioStreamMedia : public AudioStream {
	GDCLASS(AudioStreamMedia, AudioStream);
	OBJ_SAVE_TYPE(AudioStream);

	String file;
	mutable double length = -1.0;
	Dictionary http_headers;
	bool live_dvr_enabled = false;
	bool allow_http = false;
	SafeNumeric<int64_t> remote_length_usec;

protected:
	static void _bind_methods();

public:
	void set_live_dvr_enabled(bool p_enabled) { live_dvr_enabled = p_enabled; }
	bool is_live_dvr_enabled() const { return live_dvr_enabled; }
	void set_allow_http(bool p_enabled) { allow_http = p_enabled; }
	bool is_http_allowed() const { return allow_http; }
	void set_http_headers(const Dictionary &p_headers) { http_headers = p_headers.duplicate(); }
	Dictionary get_http_headers() const { return http_headers.duplicate(); }
	void publish_remote_length(double p_length) { remote_length_usec.set(int64_t(MAX(p_length, 0.0) * 1000000.0)); }
	void set_file(const String &p_file);
	String get_file() const;
	Ref<AudioStreamPlayback> instantiate_playback() override;
	double get_length() const override;
};

Ref<AudioStreamPlayback> ffmpeg_audio_playback_from_buffer(const Vector<uint8_t> &p_data, const char *p_format = nullptr, const String &p_path = String(), const Ref<AudioStream> &p_loop_stream = Ref<AudioStream>(), const Dictionary &p_headers = Dictionary(), const SafeFlag *p_cancel = nullptr, double *r_length = nullptr, FFmpegDVRWindow *p_dvr = nullptr, bool p_dvr_primary = true, bool p_allow_http = false);
Ref<VideoStreamPlayback> ffmpeg_open_video_on_worker(const String &p_path, const Dictionary &p_headers, const SafeFlag *p_cancel, const String &p_audio_file = String(), const Dictionary &p_audio_headers = Dictionary(), String *r_error = nullptr, FFmpegDVRWindow *p_dvr = nullptr, bool p_allow_http = false);
bool ffmpeg_start_video_read_ahead(const Ref<VideoStreamPlayback> &p_playback, String *r_error = nullptr, bool p_after_finite_seek = false);
bool ffmpeg_seek_video_on_worker(const Ref<VideoStreamPlayback> &p_playback, double p_position);
bool ffmpeg_can_reuse_video_seek(const Ref<VideoStreamPlayback> &p_playback);
Ref<AudioStreamPlayback> ffmpeg_audio_playback_from_ogg_packets(const Ref<OggPacketSequence> &p_sequence, const Ref<AudioStream> &p_loop_stream = Ref<AudioStream>());
bool ffmpeg_probe_audio_buffer(const Vector<uint8_t> &p_data, FFmpegAudioMetadata &r_metadata);

class ResourceFormatLoaderMedia : public ResourceFormatLoader {
	GDCLASS(ResourceFormatLoaderMedia, ResourceFormatLoader);

public:
	Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	void get_recognized_extensions(List<String> *p_extensions) const override;
	bool recognize_path(const String &p_path, const String &p_for_type = String()) const override;
	bool handles_type(const String &p_type) const override;
	String get_resource_type(const String &p_path) const override;
};

#endif
