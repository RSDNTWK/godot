#include "movie_writer_ogv_ffmpeg.h"

#include "core/io/file_access.h"
#include "core/os/memory.h"
#include "core/config/project_settings.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

uint32_t MovieWriterOGVFFmpeg::get_audio_mix_rate() const {
	return 48000;
}

AudioServer::SpeakerMode MovieWriterOGVFFmpeg::get_audio_speaker_mode() const {
	return AudioServer::SPEAKER_MODE_STEREO;
}

void MovieWriterOGVFFmpeg::get_supported_extensions(List<String> *r_extensions) const {
	r_extensions->push_back("ogv");
}

bool MovieWriterOGVFFmpeg::handles_file(const String &p_path) const {
	return p_path.has_extension("ogv");
}

void MovieWriterOGVFFmpeg::close() {
	if (format) {
		if (video_codec) {
			flush_encoder(video_codec, video_stream);
		}
		if (audio_codec) {
			flush_encoder(audio_codec, audio_stream);
		}
		if (!(format->oformat->flags & AVFMT_NOFILE) && format->pb) {
			avio_closep(&format->pb);
		}
		avformat_free_context(format);
	}
	if (resampler) {
		swr_free(&resampler);
	}
	if (scale) {
		sws_freeContext(scale);
	}
	if (video_codec) {
		avcodec_free_context(&video_codec);
	}
	if (audio_codec) {
		avcodec_free_context(&audio_codec);
	}
	format = nullptr;
	video_stream = nullptr;
	audio_stream = nullptr;
	video_frame = 0;
	audio_samples_per_frame = 0;
}

Error MovieWriterOGVFFmpeg::write_begin(const Size2i &p_movie_size, uint32_t p_fps, const String &p_base_path) {
	close();
	if ((p_movie_size.width & 1) || (p_movie_size.height & 1) || p_fps == 0) {
		return ERR_INVALID_PARAMETER;
	}
	output_path = p_base_path.get_basename() + ".ogv";
	audio_samples_per_frame = get_audio_mix_rate() / int(p_fps);
	if (output_path.is_relative_path()) {
		output_path = "res://" + output_path;
	}
	String path = ProjectSettings::get_singleton()->globalize_path(output_path);
	if (avformat_alloc_output_context2(&format, nullptr, "ogg", path.utf8().get_data()) < 0 || !format) {
		ERR_PRINT("FFmpeg OGV writer could not create the OGG output context.");
		return ERR_CANT_CREATE;
	}

	const AVCodec *video_encoder = avcodec_find_encoder_by_name("libtheora");
	const AVCodec *audio_encoder = avcodec_find_encoder(AV_CODEC_ID_VORBIS);
	if (!video_encoder || !audio_encoder) {
		ERR_PRINT("FFmpeg OGV writer could not find the libtheora or Vorbis encoder.");
		close();
		return ERR_UNAVAILABLE;
	}
	video_stream = avformat_new_stream(format, nullptr);
	audio_stream = avformat_new_stream(format, nullptr);
	if (!video_stream || !audio_stream) {
		close();
		return ERR_OUT_OF_MEMORY;
	}
	video_codec = avcodec_alloc_context3(video_encoder);
	audio_codec = avcodec_alloc_context3(audio_encoder);
	if (!video_codec || !audio_codec) {
		close();
		return ERR_OUT_OF_MEMORY;
	}
	video_codec->codec_id = AV_CODEC_ID_THEORA;
	video_codec->codec_type = AVMEDIA_TYPE_VIDEO;
	video_codec->width = p_movie_size.width;
	video_codec->height = p_movie_size.height;
	video_codec->pix_fmt = AV_PIX_FMT_YUV420P;
	video_codec->time_base = AVRational{ 1, int(p_fps) };
	video_codec->framerate = AVRational{ int(p_fps), 1 };
	video_stream->time_base = video_codec->time_base;

	audio_codec->codec_type = AVMEDIA_TYPE_AUDIO;
	audio_codec->sample_rate = get_audio_mix_rate();
	audio_codec->sample_fmt = AV_SAMPLE_FMT_FLTP;
	audio_codec->bit_rate = 128000;
	audio_codec->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
	audio_channels = 2;
	av_channel_layout_default(&audio_codec->ch_layout, audio_channels);
	audio_codec->time_base = AVRational{ 1, audio_codec->sample_rate };
	audio_stream->time_base = audio_codec->time_base;
	if (avcodec_open2(video_codec, video_encoder, nullptr) < 0 || avcodec_open2(audio_codec, audio_encoder, nullptr) < 0) {
		ERR_PRINT("FFmpeg OGV writer could not open the Theora or Vorbis encoder.");
		close();
		return ERR_UNAVAILABLE;
	}
	avcodec_parameters_from_context(video_stream->codecpar, video_codec);
	avcodec_parameters_from_context(audio_stream->codecpar, audio_codec);
	if (!(format->oformat->flags & AVFMT_NOFILE) && avio_open(&format->pb, path.utf8().get_data(), AVIO_FLAG_WRITE) < 0) {
		close();
		return ERR_CANT_CREATE;
	}
	if (avformat_write_header(format, nullptr) < 0) {
		close();
		return ERR_CANT_CREATE;
	}
	return OK;
}

Error MovieWriterOGVFFmpeg::encode_video(const Ref<Image> &p_image) {
	Ref<Image> image = p_image->duplicate();
	image->convert(Image::FORMAT_RGBA8);
	AVFrame *frame = av_frame_alloc();
	if (!frame) {
		return ERR_OUT_OF_MEMORY;
	}
	frame->format = video_codec->pix_fmt;
	frame->width = video_codec->width;
	frame->height = video_codec->height;
	if (av_frame_get_buffer(frame, 32) < 0) {
		av_frame_free(&frame);
		return ERR_OUT_OF_MEMORY;
	}
	scale = scale ? scale : sws_getContext(image->get_width(), image->get_height(), AV_PIX_FMT_RGBA, video_codec->width, video_codec->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
	if (!scale) {
		av_frame_free(&frame);
		return ERR_UNAVAILABLE;
	}
	const uint8_t *src[] = { image->get_data().ptr() };
	int stride[] = { image->get_width() * 4 };
	sws_scale(scale, src, stride, 0, image->get_height(), frame->data, frame->linesize);
	frame->pts = video_frame++;
	int result = avcodec_send_frame(video_codec, frame);
	av_frame_free(&frame);
	if (result < 0) {
		return ERR_CANT_OPEN;
	}
	AVPacket *packet = av_packet_alloc();
	while (packet && avcodec_receive_packet(video_codec, packet) >= 0) {
		av_packet_rescale_ts(packet, video_codec->time_base, video_stream->time_base);
		packet->stream_index = video_stream->index;
		av_interleaved_write_frame(format, packet);
		av_packet_unref(packet);
	}
	av_packet_free(&packet);
	return OK;
}

Error MovieWriterOGVFFmpeg::encode_audio(const int32_t *p_audio_data, int p_samples) {
	if (!p_audio_data || p_samples <= 0) {
		return OK;
	}
	if (!resampler) {
		if (swr_alloc_set_opts2(&resampler, &audio_codec->ch_layout, audio_codec->sample_fmt, audio_codec->sample_rate, &audio_codec->ch_layout, AV_SAMPLE_FMT_S32, audio_codec->sample_rate, 0, nullptr) < 0 || swr_init(resampler) < 0) {
			return ERR_UNAVAILABLE;
		}
	}
	const int frame_samples = audio_codec->frame_size > 0 ? audio_codec->frame_size : p_samples;
	for (int offset = 0; offset + frame_samples <= p_samples; offset += frame_samples) {
		AVFrame *frame = av_frame_alloc();
		if (!frame) {
			return ERR_OUT_OF_MEMORY;
		}
		frame->nb_samples = frame_samples;
		frame->format = audio_codec->sample_fmt;
		frame->ch_layout = audio_codec->ch_layout;
		frame->sample_rate = audio_codec->sample_rate;
		if (av_frame_get_buffer(frame, 0) < 0) {
			av_frame_free(&frame);
			return ERR_OUT_OF_MEMORY;
		}
		const uint8_t *input[] = { reinterpret_cast<const uint8_t *>(p_audio_data + offset * audio_channels) };
		if (swr_convert(resampler, frame->data, frame_samples, input, frame_samples) < 0) {
			av_frame_free(&frame);
			return ERR_CANT_OPEN;
		}
		frame->pts = audio_frame;
		audio_frame += frame_samples;
		int result = avcodec_send_frame(audio_codec, frame);
		av_frame_free(&frame);
		if (result < 0) {
			return ERR_CANT_OPEN;
		}
		AVPacket *packet = av_packet_alloc();
		while (packet && avcodec_receive_packet(audio_codec, packet) >= 0) {
			av_packet_rescale_ts(packet, audio_codec->time_base, audio_stream->time_base);
			packet->stream_index = audio_stream->index;
			av_interleaved_write_frame(format, packet);
			av_packet_unref(packet);
		}
		av_packet_free(&packet);
	}
	return OK;
}

Error MovieWriterOGVFFmpeg::write_frame(const Ref<Image> &p_image, const int32_t *p_audio_data) {
	if (!format || !video_codec || !audio_codec) {
		return ERR_UNAVAILABLE;
	}
	Error err = encode_video(p_image);
	if (err != OK) {
		return err;
	}
	return encode_audio(p_audio_data, audio_samples_per_frame);
}

Error MovieWriterOGVFFmpeg::flush_encoder(AVCodecContext *p_codec, AVStream *p_stream) {
	if (!p_codec || !format) {
		return OK;
	}
	avcodec_send_frame(p_codec, nullptr);
	AVPacket *packet = av_packet_alloc();
	while (packet && avcodec_receive_packet(p_codec, packet) >= 0) {
		av_packet_rescale_ts(packet, p_codec->time_base, p_stream->time_base);
		packet->stream_index = p_stream->index;
		av_interleaved_write_frame(format, packet);
		av_packet_unref(packet);
	}
	av_packet_free(&packet);
	return OK;
}

void MovieWriterOGVFFmpeg::write_end() {
	if (format) {
		flush_encoder(video_codec, video_stream);
		flush_encoder(audio_codec, audio_stream);
		av_write_trailer(format);
	}
	close();
}

MovieWriterOGVFFmpeg::~MovieWriterOGVFFmpeg() {
	close();
}
