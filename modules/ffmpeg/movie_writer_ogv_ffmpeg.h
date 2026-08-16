#ifndef MOVIE_WRITER_OGV_FFMPEG_H
#define MOVIE_WRITER_OGV_FFMPEG_H

#include "servers/movie_writer/movie_writer.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct SwsContext;
struct SwrContext;

class MovieWriterOGVFFmpeg : public MovieWriter {
	GDCLASS(MovieWriterOGVFFmpeg, MovieWriter);

	AVFormatContext *format = nullptr;
	AVCodecContext *video_codec = nullptr;
	AVCodecContext *audio_codec = nullptr;
	AVStream *video_stream = nullptr;
	AVStream *audio_stream = nullptr;
	SwsContext *scale = nullptr;
	SwrContext *resampler = nullptr;
	String output_path;
	uint64_t video_frame = 0;
	int64_t audio_frame = 0;
	int audio_channels = 2;
	int audio_samples_per_frame = 0;

	void close();
	Error encode_video(const Ref<Image> &p_image);
	Error encode_audio(const int32_t *p_audio_data, int p_samples);
	Error flush_encoder(AVCodecContext *p_codec, AVStream *p_stream);

protected:
	uint32_t get_audio_mix_rate() const override;
	AudioServer::SpeakerMode get_audio_speaker_mode() const override;
	void get_supported_extensions(List<String> *r_extensions) const override;
	Error write_begin(const Size2i &p_movie_size, uint32_t p_fps, const String &p_base_path) override;
	Error write_frame(const Ref<Image> &p_image, const int32_t *p_audio_data) override;
	void write_end() override;
	bool handles_file(const String &p_path) const override;

public:
	~MovieWriterOGVFFmpeg() override;
};

#endif
