#include "movie_writer_ogv_ffmpeg.h"

#include "ffmpeg_video_frame_converter.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

static const AVCodec *ogv_audio_encoder() {
	const AVCodec *codec = avcodec_find_encoder_by_name("libvorbis");
	return codec ? codec : avcodec_find_encoder(AV_CODEC_ID_VORBIS);
}
bool MovieWriterOGVFFmpeg::is_available() {
	const AVCodec *audio = ogv_audio_encoder();
	// The native FFmpeg Vorbis encoder accepts stereo only; the native Godot
	// recorder remains available for the other supported speaker modes.
	return av_guess_format("ogg", nullptr, nullptr) && avcodec_find_encoder_by_name("libtheora") &&
			audio && (avcodec_find_encoder_by_name("libvorbis") || int(GLOBAL_GET("editor/movie_writer/speaker_mode")) == 0);
}
uint32_t MovieWriterOGVFFmpeg::get_audio_mix_rate() const {
	return GLOBAL_GET("editor/movie_writer/mix_rate");
}
AudioServer::SpeakerMode MovieWriterOGVFFmpeg::get_audio_speaker_mode() const {
	return AudioServer::SpeakerMode(int(GLOBAL_GET("editor/movie_writer/speaker_mode")));
}
void MovieWriterOGVFFmpeg::get_supported_extensions(List<String> *r_extensions) const {
	r_extensions->push_back("ogv");
}
bool MovieWriterOGVFFmpeg::handles_file(const String &p_path) const {
	return p_path.has_extension("ogv");
}

Error MovieWriterOGVFFmpeg::fail(Error p_error) {
	set_error(p_error);
	close();
	return p_error;
}
void MovieWriterOGVFFmpeg::close() {
	// Cleanup never sends frames or writes a trailer. Only write_end finalizes
	// a successfully initialized recording, once.
	if (format) {
		if (format->pb && !(format->oformat->flags & AVFMT_NOFILE)) {
			avio_flush(format->pb);
			if (format->pb->error < 0) {
				set_error(ERR_FILE_CANT_WRITE);
			}
			if (avio_closep(&format->pb) < 0) {
				set_error(ERR_FILE_CANT_WRITE);
			}
		}
		avformat_free_context(format);
		format = nullptr;
	}
	swr_free(&resampler);
	if (scale) {
		sws_freeContext(scale);
		scale = nullptr;
	}
	if (audio_fifo) {
		av_audio_fifo_free(audio_fifo);
		audio_fifo = nullptr;
	}
	avcodec_free_context(&video_codec);
	avcodec_free_context(&audio_codec);
	video_stream = audio_stream = nullptr;
	video_frame = audio_frame = 0;
	audio_samples_per_frame = 0;
	active = false;
	audio_packet_written = false;
}

Error MovieWriterOGVFFmpeg::write_begin(const Size2i &p_movie_size, uint32_t p_fps, const String &p_base_path) {
	close();
	reset_error();
	const uint32_t mix_rate = get_audio_mix_rate();
	const AudioServer::SpeakerMode mode = get_audio_speaker_mode();
	const double video_quality = GLOBAL_GET("editor/movie_writer/video_quality");
	const double audio_quality = GLOBAL_GET("editor/movie_writer/ogv/audio_quality");
	const int speed = GLOBAL_GET("editor/movie_writer/ogv/encoding_speed");
	const int keyframes = GLOBAL_GET("editor/movie_writer/ogv/keyframe_interval");
	int64_t rgba_size = 0;
	if (p_fps == 0 || p_fps > INT32_MAX || p_movie_size.width <= 0 || p_movie_size.height <= 0 ||
			(p_movie_size.width & 1) || (p_movie_size.height & 1) ||
			FFmpegVideoFrameConverter::get_rgba_size(p_movie_size.width, p_movie_size.height, rgba_size) != OK ||
			mix_rate < 8000 || mix_rate > 192000 || mix_rate / p_fps == 0 ||
			mode < AudioServer::SPEAKER_MODE_STEREO || mode > AudioServer::SPEAKER_SURROUND_71 ||
			!Math::is_finite(video_quality) || video_quality < 0 || video_quality > 1 ||
			!Math::is_finite(audio_quality) || audio_quality < -0.1 || audio_quality > 1 ||
			speed < 1 || speed > 4 || keyframes < 1 || keyframes > 1024) {
		return fail(ERR_INVALID_PARAMETER);
	}
	const AVCodec *video_encoder = avcodec_find_encoder_by_name("libtheora");
	const AVCodec *audio_encoder = ogv_audio_encoder();
	const int input_audio_channels = (int(mode) + 1) * 2;
	audio_channels = input_audio_channels == 4 ? 6 : input_audio_channels;
	if (!av_guess_format("ogg", nullptr, nullptr) || !video_encoder || !audio_encoder ||
			(!avcodec_find_encoder_by_name("libvorbis") && input_audio_channels != 2)) {
		return fail(ERR_UNAVAILABLE);
	}
	movie_size = p_movie_size;
	audio_samples_per_frame = mix_rate / p_fps;
	output_path = p_base_path.get_basename() + ".ogv";
	if (output_path.is_relative_path()) {
		output_path = "res://" + output_path;
	}
	const String path = ProjectSettings::get_singleton()->globalize_path(output_path);
	if (avformat_alloc_output_context2(&format, nullptr, "ogg", path.utf8().get_data()) < 0 || !format) {
		return fail(ERR_CANT_CREATE);
	}
	video_stream = avformat_new_stream(format, nullptr);
	audio_stream = avformat_new_stream(format, nullptr);
	video_codec = avcodec_alloc_context3(video_encoder);
	audio_codec = avcodec_alloc_context3(audio_encoder);
	if (!video_stream || !audio_stream || !video_codec || !audio_codec) {
		return fail(ERR_OUT_OF_MEMORY);
	}
	video_codec->width = movie_size.width;
	video_codec->height = movie_size.height;
	video_codec->pix_fmt = AV_PIX_FMT_YUV420P;
	video_codec->time_base = { 1, int(p_fps) };
	video_codec->framerate = { int(p_fps), 1 };
	video_codec->gop_size = keyframes;
	video_codec->flags |= AV_CODEC_FLAG_QSCALE | AV_CODEC_FLAG_FRAME_DURATION;
	video_codec->global_quality = int(video_quality * 10 * FF_QP2LAMBDA);
	video_stream->time_base = video_codec->time_base;
	audio_codec->sample_rate = mix_rate;
	audio_codec->sample_fmt = AV_SAMPLE_FMT_FLTP;
	audio_codec->bit_rate = 128000;
	audio_codec->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
	audio_codec->flags |= AV_CODEC_FLAG_FRAME_DURATION;
	// libvorbis accepts the setting's -0.1..1 scale. The native encoder
	// uses a positive 1..10 quality factor and ignores bitrate mode.
	double quality = audio_quality * 10.0;
	if (avcodec_find_encoder_by_name("libvorbis") != audio_encoder) {
		// Keep the native encoder's established default of 8 at setting 0.5,
		// while making both ends of the accepted setting range effective.
		quality = audio_quality <= 0.5 ? 1.0 + 7.0 * (audio_quality + 0.1) / 0.6 : 8.0 + 4.0 * (audio_quality - 0.5);
	}
	audio_codec->flags |= AV_CODEC_FLAG_QSCALE;
	audio_codec->global_quality = int(quality * FF_QP2LAMBDA);
	av_channel_layout_default(&audio_codec->ch_layout, audio_channels);
	audio_codec->time_base = { 1, int(mix_rate) };
	audio_stream->time_base = audio_codec->time_base;
	if (format->oformat->flags & AVFMT_GLOBALHEADER) {
		video_codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		audio_codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}
	if (av_opt_set_int(video_codec->priv_data, "speed_level", speed, 0) < 0 ||
			avcodec_open2(video_codec, video_encoder, nullptr) < 0 ||
			avcodec_open2(audio_codec, audio_encoder, nullptr) < 0) {
		return fail(ERR_UNAVAILABLE);
	}
	if (audio_codec->frame_size < 0 || audio_codec->frame_size > 262144) {
		return fail(ERR_UNAVAILABLE);
	}
	if (avcodec_parameters_from_context(video_stream->codecpar, video_codec) < 0 ||
			avcodec_parameters_from_context(audio_stream->codecpar, audio_codec) < 0) {
		return fail(ERR_OUT_OF_MEMORY);
	}
	AVChannelLayout input_layout = {};
	av_channel_layout_default(&input_layout, input_audio_channels);
	if (input_audio_channels == 4) {
		input_layout = AV_CHANNEL_LAYOUT_3POINT1;
	}
	const int resampler_result = swr_alloc_set_opts2(&resampler, &audio_codec->ch_layout, audio_codec->sample_fmt, audio_codec->sample_rate,
			&input_layout, AV_SAMPLE_FMT_S32, audio_codec->sample_rate, 0, nullptr);
	av_channel_layout_uninit(&input_layout);
	if (resampler_result < 0 || !resampler || swr_init(resampler) < 0) {
		return fail(ERR_UNAVAILABLE);
	}
	audio_fifo = av_audio_fifo_alloc(audio_codec->sample_fmt, audio_channels, MAX(1, audio_codec->frame_size));
	if (!audio_fifo) {
		return fail(ERR_OUT_OF_MEMORY);
	}
	if (!(format->oformat->flags & AVFMT_NOFILE) && avio_open(&format->pb, path.utf8().get_data(), AVIO_FLAG_WRITE) < 0) {
		return fail(ERR_CANT_CREATE);
	}
	if (avformat_write_header(format, nullptr) < 0 || (format->pb && format->pb->error < 0)) {
		return fail(ERR_FILE_CANT_WRITE);
	}
	active = true;
	return OK;
}

Error MovieWriterOGVFFmpeg::drain_packets(AVCodecContext *p_codec, AVStream *p_stream, bool p_final) {
	AVPacket *packet = av_packet_alloc();
	if (!packet) {
		return ERR_OUT_OF_MEMORY;
	}
	Error error = OK;
	while (true) {
		const int result = avcodec_receive_packet(p_codec, packet);
		if (result == AVERROR_EOF) {
			if (!p_final) {
				error = ERR_CANT_CREATE;
			}
			break;
		}
		if (result == AVERROR(EAGAIN)) {
			if (p_final) {
				error = ERR_CANT_CREATE;
			}
			break;
		}
		if (result < 0) {
			error = ERR_CANT_CREATE;
			break;
		}
		// Some encoders pad the final small frame. Ogg uses pts + duration as
		// its ending granule, so retain only the samples supplied by the FIFO.
		if (p_codec == audio_codec && p_final && packet->pts >= 0) {
			if (packet->pts > audio_frame || packet->duration < 0) {
				error = ERR_INVALID_DATA;
				break;
			}
			packet->duration = MIN(packet->duration, audio_frame - packet->pts);
		}
		av_packet_rescale_ts(packet, p_codec->time_base, p_stream->time_base);
		packet->stream_index = p_stream->index;
		const bool first_audio = p_codec == audio_codec && !audio_packet_written;
		int64_t page_duration = 0;
		if (first_audio && (av_opt_get_int(format->priv_data, "page_duration", 0, &page_duration) < 0 || av_opt_set_int(format->priv_data, "page_duration", 1, 0) < 0)) {
			error = ERR_CANT_CREATE;
			break;
		}
		int written = av_interleaved_write_frame(format, packet);
		av_packet_unref(packet);
		if (first_audio) {
			// Flush the initial preroll packet as a separate page, including any
			// interleaver delay, then restore ordinary page aggregation.
			if (written >= 0) {
				written = av_interleaved_write_frame(format, nullptr);
			}
			const int restored = av_opt_set_int(format->priv_data, "page_duration", page_duration, 0);
			if (restored < 0 && written >= 0) {
				written = restored;
			}
			audio_packet_written = written >= 0;
		}
		if (written < 0 || (format->pb && format->pb->error < 0)) {
			error = ERR_FILE_CANT_WRITE;
			break;
		}
	}
	av_packet_free(&packet);
	return error;
}
Error MovieWriterOGVFFmpeg::send_frame(AVCodecContext *p_codec, AVStream *p_stream, AVFrame *p_frame) {
	for (int attempt = 0; attempt < 2; attempt++) {
		const int result = avcodec_send_frame(p_codec, p_frame);
		if (result == AVERROR(EAGAIN)) {
			const Error drained = drain_packets(p_codec, p_stream, false);
			if (drained != OK) {
				return drained;
			}
			continue; // Retry this same retained frame.
		}
		if (result < 0 && !(p_frame == nullptr && result == AVERROR_EOF)) {
			return ERR_CANT_CREATE;
		}
		return drain_packets(p_codec, p_stream, p_frame == nullptr);
	}
	return ERR_CANT_CREATE;
}

Error MovieWriterOGVFFmpeg::encode_video(const Ref<Image> &p_image) {
	if (p_image.is_null() || p_image->is_empty() || p_image->get_size() != movie_size || video_frame == INT64_MAX) {
		return ERR_INVALID_PARAMETER;
	}
	Ref<Image> image = p_image;
	if (image->get_format() != Image::FORMAT_RGBA8) {
		image = p_image->duplicate();
		if (image.is_null()) {
			return ERR_OUT_OF_MEMORY;
		}
		image->convert(Image::FORMAT_RGBA8);
		if (image->is_empty() || image->get_format() != Image::FORMAT_RGBA8 || image->get_size() != movie_size) {
			return ERR_CANT_CREATE;
		}
	}
	int64_t source_size = 0;
	if (FFmpegVideoFrameConverter::get_rgba_size(movie_size.width, movie_size.height, source_size) != OK) {
		return ERR_INVALID_PARAMETER;
	}
	const Vector<uint8_t> pixels = image->get_data();
	if (pixels.size() < source_size) {
		return ERR_INVALID_DATA;
	}
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
	if (!scale) {
		scale = sws_getContext(movie_size.width, movie_size.height, AV_PIX_FMT_RGBA, movie_size.width, movie_size.height, video_codec->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
	}
	if (!scale) {
		av_frame_free(&frame);
		return ERR_UNAVAILABLE;
	}
	const uint8_t *source[4] = { pixels.ptr(), nullptr, nullptr, nullptr };
	const int strides[4] = { movie_size.width * 4, 0, 0, 0 };
	if (sws_scale(scale, source, strides, 0, movie_size.height, frame->data, frame->linesize) != movie_size.height) {
		av_frame_free(&frame);
		return ERR_CANT_CREATE;
	}
	frame->pts = video_frame;
	frame->duration = 1;
	const Error error = send_frame(video_codec, video_stream, frame);
	av_frame_free(&frame);
	if (error == OK) {
		video_frame++;
	}
	return error;
}

Error MovieWriterOGVFFmpeg::convert_audio(const int32_t *p_audio_data, int p_samples, bool p_flush, int &r_converted) {
	r_converted = 0;
	if (p_samples < 0 || (!p_flush && (p_samples == 0 || !p_audio_data))) {
		return ERR_INVALID_PARAMETER;
	}
	const int capacity = swr_get_out_samples(resampler, p_flush ? 0 : p_samples);
	if (capacity < 0 || capacity > 262144) {
		return ERR_CANT_CREATE;
	}
	const uint8_t *input[AV_NUM_DATA_POINTERS] = { reinterpret_cast<const uint8_t *>(p_audio_data) };
	if (capacity == 0) {
		const int result = swr_convert(resampler, nullptr, 0, p_flush ? nullptr : input, p_flush ? 0 : p_samples);
		return result < 0 ? ERR_CANT_CREATE : OK;
	}
	AVFrame *frame = av_frame_alloc();
	if (!frame) {
		return ERR_OUT_OF_MEMORY;
	}
	frame->format = audio_codec->sample_fmt;
	frame->sample_rate = audio_codec->sample_rate;
	frame->nb_samples = capacity;
	if (av_channel_layout_copy(&frame->ch_layout, &audio_codec->ch_layout) < 0 || av_frame_get_buffer(frame, 0) < 0) {
		av_frame_free(&frame);
		return ERR_OUT_OF_MEMORY;
	}
	const int converted = swr_convert(resampler, frame->extended_data, capacity, p_flush ? nullptr : input, p_flush ? 0 : p_samples);
	if (converted < 0 || converted > capacity) {
		av_frame_free(&frame);
		return ERR_CANT_CREATE;
	}
	const int buffered = av_audio_fifo_size(audio_fifo);
	Error error = OK;
	if (buffered < 0 || buffered > 524288 - converted) {
		error = ERR_CANT_CREATE;
	} else if (converted > 0) {
		if (av_audio_fifo_realloc(audio_fifo, buffered + converted) < 0) {
			error = ERR_OUT_OF_MEMORY;
		} else if (av_audio_fifo_write(audio_fifo, reinterpret_cast<void **>(frame->extended_data), converted) != converted) {
			error = ERR_CANT_CREATE;
		}
	}
	av_frame_free(&frame);
	if (error == OK) {
		r_converted = converted;
	}
	return error;
}

Error MovieWriterOGVFFmpeg::emit_audio(bool p_final) {
	while (true) {
		const int available = av_audio_fifo_size(audio_fifo);
		if (available < 0) {
			return ERR_CANT_CREATE;
		}
		if (available == 0 || (!p_final && audio_codec->frame_size > 0 && available < audio_codec->frame_size)) {
			return OK;
		}
		const int count = MIN(available, audio_codec->frame_size > 0 ? audio_codec->frame_size : 4096);
		if (audio_frame > INT64_MAX - count) {
			return ERR_INVALID_PARAMETER;
		}
		AVFrame *frame = av_frame_alloc();
		if (!frame) {
			return ERR_OUT_OF_MEMORY;
		}
		frame->format = audio_codec->sample_fmt;
		frame->sample_rate = audio_codec->sample_rate;
		frame->nb_samples = count;
		frame->pts = audio_frame;
		frame->duration = count;
		if (av_channel_layout_copy(&frame->ch_layout, &audio_codec->ch_layout) < 0 || av_frame_get_buffer(frame, 0) < 0) {
			av_frame_free(&frame);
			return ERR_OUT_OF_MEMORY;
		}
		if (av_audio_fifo_read(audio_fifo, reinterpret_cast<void **>(frame->extended_data), count) != count) {
			av_frame_free(&frame);
			return ERR_CANT_CREATE;
		}
		// The pinned encoder API accepts one undersized final frame and records
		// any necessary padding. No remainder is dropped or sent twice.
		const Error error = send_frame(audio_codec, audio_stream, frame);
		av_frame_free(&frame);
		if (error != OK) {
			return error;
		}
		audio_frame += count;
	}
}

Error MovieWriterOGVFFmpeg::write_frame(const Ref<Image> &p_image, const int32_t *p_audio_data) {
	if (get_error() != OK) {
		return get_error();
	}
	if (!active || !p_audio_data) {
		return fail(ERR_UNCONFIGURED);
	}
	Error error = encode_video(p_image);
	int converted = 0;
	if (error == OK) {
		error = convert_audio(p_audio_data, audio_samples_per_frame, false, converted);
	}
	if (error == OK) {
		error = emit_audio(false);
	}
	return error == OK ? OK : fail(error);
}

void MovieWriterOGVFFmpeg::write_end() {
	if (!active) {
		close();
		return;
	}
	active = false;
	Error error = get_error();
	if (error == OK) {
		bool drained = false;
		for (int batch = 0; batch < 16; batch++) {
			int converted = 0;
			error = convert_audio(nullptr, 0, true, converted);
			if (error != OK) {
				break;
			}
			if (converted == 0) {
				drained = true;
				break;
			}
			error = emit_audio(false);
			if (error != OK) {
				break;
			}
		}
		if (error == OK && !drained) {
			error = ERR_CANT_CREATE;
		}
		if (error == OK) {
			error = emit_audio(true);
		}
		if (error == OK && video_frame > 0) {
			error = send_frame(video_codec, video_stream, nullptr);
		}
		if (error == OK && audio_frame > 0) {
			error = send_frame(audio_codec, audio_stream, nullptr);
		}
		if (error == OK && av_write_trailer(format) < 0) {
			error = ERR_FILE_CANT_WRITE;
		}
	}
	set_error(error);
	close();
}

#ifdef DEV_ENABLED
static int ogv_fail_output(void *, const uint8_t *, int) {
	return AVERROR(EIO);
}
void MovieWriterOGVFFmpeg::test_fail_output_writes() {
	ERR_FAIL_COND(!active || !format || !format->pb);
	avio_flush(format->pb);
	format->pb->write_packet = ogv_fail_output;
	format->pb->direct = 1;
}
#endif

MovieWriterOGVFFmpeg::~MovieWriterOGVFFmpeg() {
	close();
}
