#ifndef VIDEO_STREAM_MEDIA_H
#define VIDEO_STREAM_MEDIA_H

#include "scene/resources/video_stream.h"
#include "core/io/resource_loader.h"
#include "servers/audio/audio_stream.h"

class OggPacketSequence;

class VideoStreamPlaybackFFmpeg;

struct FFmpegAudioMetadata {
	int channels = 0;
	int sample_rate = 0;
	double length = 0.0;
};

class VideoStreamMedia : public VideoStream {
    GDCLASS(VideoStreamMedia, VideoStream);

protected:
    static void _bind_methods();

public:
    Ref<VideoStreamPlayback> instantiate_playback() override;
};

class AudioStreamMedia : public AudioStream {
	GDCLASS(AudioStreamMedia, AudioStream);
	OBJ_SAVE_TYPE(AudioStream);

	String file;

protected:
	static void _bind_methods();

public:
	void set_file(const String &p_file);
	String get_file() const;
	Ref<AudioStreamPlayback> instantiate_playback() override;
	double get_length() const override;
};

Ref<AudioStreamPlayback> ffmpeg_audio_playback_from_buffer(const Vector<uint8_t> &p_data, const char *p_format = nullptr, const String &p_path = String());
Ref<AudioStreamPlayback> ffmpeg_audio_playback_from_ogg_packets(const Ref<OggPacketSequence> &p_sequence);
bool ffmpeg_probe_audio_buffer(const Vector<uint8_t> &p_data, FFmpegAudioMetadata &r_metadata);

class ResourceFormatLoaderMedia : public ResourceFormatLoader {
    GDCLASS(ResourceFormatLoaderMedia, ResourceFormatLoader);

public:
    Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
    void get_recognized_extensions(List<String> *p_extensions) const override;
    bool handles_type(const String &p_type) const override;
    String get_resource_type(const String &p_path) const override;
};

#endif
