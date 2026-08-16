#include "video_stream_media.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/object/class_db.h"
#include "modules/ogg/ogg_packet_sequence.h"
#include "servers/audio/audio_server.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswresample/swresample.h>
}

#include <ogg/ogg.h>

#include <cstring>

struct FFmpegBufferIO {
	const uint8_t *data = nullptr;
	int64_t size = 0;
	int64_t position = 0;
};

static int _ffmpeg_buffer_read(void *p_opaque, uint8_t *p_buffer, int p_buffer_size) {
	FFmpegBufferIO *io = static_cast<FFmpegBufferIO *>(p_opaque);
	int64_t available = io->size - io->position;
	if (available <= 0) {
		return AVERROR_EOF;
	}
	int amount = MIN(int64_t(p_buffer_size), available);
	memcpy(p_buffer, io->data + io->position, amount);
	io->position += amount;
	return amount;
}

static int64_t _ffmpeg_buffer_seek(void *p_opaque, int64_t p_offset, int p_whence) {
	FFmpegBufferIO *io = static_cast<FFmpegBufferIO *>(p_opaque);
	if ((p_whence & AVSEEK_SIZE) != 0) {
		return io->size;
	}
	p_whence &= ~AVSEEK_FORCE;
	int64_t position = p_offset;
	if (p_whence == SEEK_CUR) {
		position += io->position;
	} else if (p_whence == SEEK_END) {
		position += io->size;
	}
	if (position < 0 || position > io->size) {
		return AVERROR(EINVAL);
	}
	io->position = position;
	return position;
}

class AudioStreamPlaybackFFmpeg : public AudioStreamPlayback {
	GDCLASS(AudioStreamPlaybackFFmpeg, AudioStreamPlayback);

	Vector<AudioFrame> samples;
	uint64_t frame_position = 0;
	bool playing = false;
	int output_sample_rate = 44100;

	bool open_raw_aac(const Vector<uint8_t> &p_data) {
		const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_AAC);
		if (!decoder) {
			return false;
		}
		AVCodecContext *aac_codec = avcodec_alloc_context3(decoder);
		AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_AAC);
		SwrContext *aac_resampler = nullptr;
		AVFrame *aac_frame = av_frame_alloc();
		AVPacket *aac_packet = av_packet_alloc();
		if (!aac_codec || !parser || !aac_frame || !aac_packet) {
			if (aac_codec) avcodec_free_context(&aac_codec);
			if (parser) av_parser_close(parser);
			if (aac_frame) av_frame_free(&aac_frame);
			if (aac_packet) av_packet_free(&aac_packet);
			return false;
		}
		aac_codec->sample_rate = 48000;
		av_channel_layout_default(&aac_codec->ch_layout, 2);
		if (avcodec_open2(aac_codec, decoder, nullptr) < 0) {
			avcodec_free_context(&aac_codec);
			av_parser_close(parser);
			av_frame_free(&aac_frame);
			av_packet_free(&aac_packet);
			return false;
		}
		AVChannelLayout output_layout;
		av_channel_layout_default(&output_layout, 2);
		bool ok = swr_alloc_set_opts2(&aac_resampler, &output_layout, AV_SAMPLE_FMT_FLT, output_sample_rate, &aac_codec->ch_layout, AV_SAMPLE_FMT_FLTP, 48000, 0, nullptr) >= 0 && aac_resampler && swr_init(aac_resampler) >= 0;
		av_channel_layout_uninit(&output_layout);
		if (!ok) {
			avcodec_free_context(&aac_codec);
			av_parser_close(parser);
			av_frame_free(&aac_frame);
			av_packet_free(&aac_packet);
			if (aac_resampler) swr_free(&aac_resampler);
			return false;
		}
		auto receive = [&]() {
			while (avcodec_receive_frame(aac_codec, aac_frame) >= 0) {
				int output_frames = swr_get_out_samples(aac_resampler, aac_frame->nb_samples);
				Vector<float> output;
				output.resize(output_frames * 2);
				uint8_t *output_data[] = { reinterpret_cast<uint8_t *>(output.ptrw()) };
				int converted = swr_convert(aac_resampler, output_data, output_frames, const_cast<const uint8_t **>(aac_frame->extended_data), aac_frame->nb_samples);
				for (int i = 0; i < converted; i++) {
					AudioFrame sample;
					sample.left = output[i * 2];
					sample.right = output[i * 2 + 1];
					samples.push_back(sample);
				}
			}
		};
		const uint8_t *input = p_data.ptr();
		int remaining = p_data.size();
		while (remaining > 0) {
			uint8_t *packet_data = nullptr;
			int packet_size = 0;
			int consumed = av_parser_parse2(parser, aac_codec, &packet_data, &packet_size, input, remaining, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
			if (consumed < 0) break;
			input += consumed;
			remaining -= consumed;
			if (packet_size > 0) {
				aac_packet->data = packet_data;
				aac_packet->size = packet_size;
				if (avcodec_send_packet(aac_codec, aac_packet) >= 0) receive();
			}
			if (consumed == 0) break;
		}
		avcodec_send_packet(aac_codec, nullptr);
		receive();
		av_packet_free(&aac_packet);
		av_frame_free(&aac_frame);
		av_parser_close(parser);
		avcodec_free_context(&aac_codec);
		swr_free(&aac_resampler);
		return !samples.is_empty();
	}

	public:
	bool open_buffer(const Vector<uint8_t> &p_data, const char *p_format_name, const String &p_path) {
		samples.clear();
		output_sample_rate = MAX(1, int(AudioServer::get_singleton()->get_mix_rate()));
		if (p_format_name && strcmp(p_format_name, "aac") == 0) {
			return open_raw_aac(p_data);
		}
		AVFormatContext *format = nullptr;
		AVIOContext *io_context = nullptr;
		AVCodecContext *codec = nullptr;
		AVPacket *packet = nullptr;
		AVFrame *frame = nullptr;
		SwrContext *resampler = nullptr;
		bool success = false;
		int packet_count = 0;
		int decoded_frame_count = 0;
		FFmpegBufferIO io;
		io.data = p_data.ptr();
		io.size = p_data.size();
		auto cleanup = [&]() {
			if (resampler) {
				swr_free(&resampler);
			}
			if (frame) {
				av_frame_free(&frame);
			}
			if (packet) {
				av_packet_free(&packet);
			}
			if (codec) {
				avcodec_free_context(&codec);
			}
			if (format) {
				avformat_close_input(&format);
			}
			if (io_context) {
				avio_context_free(&io_context);
			}
		};

		const AVInputFormat *input_format = p_format_name ? av_find_input_format(p_format_name) : nullptr;
		if (!p_path.is_empty()) {
			String path = ProjectSettings::get_singleton()->globalize_path(p_path);
			if (avformat_open_input(&format, path.utf8().get_data(), input_format, nullptr) < 0) {
				cleanup();
				return false;
			}
		} else {
			uint8_t *io_buffer = static_cast<uint8_t *>(av_malloc(32768));
			format = avformat_alloc_context();
			if (!io_buffer || !format) {
				cleanup();
				if (io_buffer) {
					av_free(io_buffer);
				}
				return false;
			}
			io_context = avio_alloc_context(io_buffer, 32768, 0, &io, _ffmpeg_buffer_read, nullptr, _ffmpeg_buffer_seek);
			if (!io_context) {
				cleanup();
				return false;
			}
			io_context->seekable = AVIO_SEEKABLE_NORMAL;
			format->pb = io_context;
			format->flags |= AVFMT_FLAG_CUSTOM_IO;
			format->iformat = input_format;
			if (avformat_open_input(&format, nullptr, input_format, nullptr) < 0) {
				cleanup();
				return false;
			}
		}
		if (avformat_find_stream_info(format, nullptr) < 0) {
			cleanup();
			return false;
		}
		int stream_index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
		if (stream_index < 0) {
			cleanup();
			return false;
		}
		const AVCodecParameters *params = format->streams[stream_index]->codecpar;
		const AVCodec *decoder = avcodec_find_decoder(params->codec_id);
		if (!decoder) {
			cleanup();
			return false;
		}
		codec = avcodec_alloc_context3(decoder);
		if (!codec || avcodec_parameters_to_context(codec, params) < 0 || avcodec_open2(codec, decoder, nullptr) < 0) {
			cleanup();
			return false;
		}
		if (codec->ch_layout.nb_channels == 0 || av_channel_layout_check(&codec->ch_layout) != 1) {
			av_channel_layout_uninit(&codec->ch_layout);
			av_channel_layout_default(&codec->ch_layout, 2);
		}
		AVChannelLayout output_layout;
		av_channel_layout_default(&output_layout, 2);
		if (swr_alloc_set_opts2(&resampler, &output_layout, AV_SAMPLE_FMT_FLT, output_sample_rate, &codec->ch_layout, codec->sample_fmt, codec->sample_rate, 0, nullptr) < 0 || !resampler || swr_init(resampler) < 0) {
			av_channel_layout_uninit(&output_layout);
			cleanup();
			return false;
		}
		av_channel_layout_uninit(&output_layout);
		packet = av_packet_alloc();
		frame = av_frame_alloc();
		if (!packet || !frame) {
			cleanup();
			return false;
		}
			auto decode_frames = [&]() {
			while (avcodec_receive_frame(codec, frame) >= 0) {
				decoded_frame_count++;
				int output_frames = swr_get_out_samples(resampler, frame->nb_samples);
				Vector<float> output;
				output.resize(output_frames * 2);
				uint8_t *output_data[] = { reinterpret_cast<uint8_t *>(output.ptrw()) };
				int converted = swr_convert(resampler, output_data, output_frames, const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
				for (int i = 0; i < converted; i++) {
					AudioFrame sample;
					sample.left = output[i * 2];
					sample.right = output[i * 2 + 1];
			samples.push_back(sample);
				}
			}
		};

		while (av_read_frame(format, packet) >= 0) {
			packet_count++;
			if (packet->stream_index == stream_index) {
				if (avcodec_send_packet(codec, packet) >= 0) {
					decode_frames();
				}
			}
			av_packet_unref(packet);
		}
		if (avcodec_send_packet(codec, nullptr) >= 0) {
			decode_frames();
			const int flush_frames = swr_get_out_samples(resampler, 0);
			if (flush_frames > 0) {
				Vector<float> output;
				output.resize(flush_frames * 2);
				uint8_t *output_data[] = { reinterpret_cast<uint8_t *>(output.ptrw()) };
				const int converted = swr_convert(resampler, output_data, flush_frames, nullptr, 0);
				for (int i = 0; i < converted; i++) {
					AudioFrame sample;
					sample.left = output[i * 2];
					sample.right = output[i * 2 + 1];
					samples.push_back(sample);
				}
			}
		}
		success = !samples.is_empty();
		cleanup();
		return success;
	}

	bool open_file(const String &p_path) {
		String path = ProjectSettings::get_singleton()->globalize_path(p_path);
		Vector<uint8_t> data = FileAccess::get_file_as_bytes(path);
		return !data.is_empty() && open_buffer(data, nullptr, p_path);
	}

	void start(double p_from_pos = 0.0) override {
		frame_position = MIN(uint64_t(MAX(p_from_pos, 0.0) * output_sample_rate), uint64_t(samples.size()));
		playing = true;
	}
	void stop() override { playing = false; }
	bool is_playing() const override { return playing; }
	int get_loop_count() const override { return 0; }
	double get_playback_position() const override { return double(frame_position) / output_sample_rate; }
	void seek(double p_time) override { frame_position = MIN(uint64_t(MAX(p_time, 0.0) * output_sample_rate), uint64_t(samples.size())); }
	int mix(AudioFrame *p_buffer, float, int p_frames) override {
		if (!playing) {
			return 0;
		}
		int mixed = 0;
		while (mixed < p_frames && frame_position < uint64_t(samples.size())) {
			p_buffer[mixed++] = samples[frame_position++];
		}
		if (frame_position >= uint64_t(samples.size())) {
			playing = false;
		}
		return mixed;
	}
};

void AudioStreamMedia::set_file(const String &p_file) {
	file = p_file;
}

String AudioStreamMedia::get_file() const {
	return file;
}

Ref<AudioStreamPlayback> AudioStreamMedia::instantiate_playback() {
	String path = ProjectSettings::get_singleton()->globalize_path(file);
	Vector<uint8_t> data = FileAccess::get_file_as_bytes(path);
	const char *format = file.get_extension().to_lower() == "aac" ? "aac" : nullptr;
	return ffmpeg_audio_playback_from_buffer(data, format, format ? String() : file);
}

Ref<AudioStreamPlayback> ffmpeg_audio_playback_from_buffer(const Vector<uint8_t> &p_data, const char *p_format, const String &p_path) {
	Ref<AudioStreamPlaybackFFmpeg> playback;
	playback.instantiate();
	if (!playback->open_buffer(p_data, p_format, p_path)) {
		return Ref<AudioStreamPlayback>();
	}
	return playback;
}

bool ffmpeg_probe_audio_buffer(const Vector<uint8_t> &p_data, FFmpegAudioMetadata &r_metadata) {
	if (p_data.is_empty()) {
		return false;
	}
	FFmpegBufferIO io;
	io.data = p_data.ptr();
	io.size = p_data.size();
	AVFormatContext *format = avformat_alloc_context();
	if (!format) {
		return false;
	}
	uint8_t *io_buffer = static_cast<uint8_t *>(av_malloc(32768));
	if (!io_buffer) {
		avformat_free_context(format);
		return false;
	}
	AVIOContext *io_context = avio_alloc_context(io_buffer, 32768, 0, &io, _ffmpeg_buffer_read, nullptr, _ffmpeg_buffer_seek);
	if (!io_context) {
		av_free(io_buffer);
		avformat_free_context(format);
		return false;
	}
	io_context->seekable = AVIO_SEEKABLE_NORMAL;
	format->pb = io_context;
	format->flags |= AVFMT_FLAG_CUSTOM_IO;
	bool success = avformat_open_input(&format, nullptr, nullptr, nullptr) >= 0 && avformat_find_stream_info(format, nullptr) >= 0;
	if (success) {
		int stream_index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
		if (stream_index >= 0) {
			const AVCodecParameters *params = format->streams[stream_index]->codecpar;
			r_metadata.channels = params->ch_layout.nb_channels;
			r_metadata.sample_rate = params->sample_rate;
			int64_t duration = format->streams[stream_index]->duration;
			if (duration == AV_NOPTS_VALUE) {
				duration = format->duration;
				r_metadata.length = duration > 0 ? double(duration) / AV_TIME_BASE : 0.0;
			} else {
				r_metadata.length = duration * av_q2d(format->streams[stream_index]->time_base);
			}
			success = r_metadata.channels > 0 && r_metadata.sample_rate > 0;
		} else {
			success = false;
		}
	}
	if (format) {
		avformat_close_input(&format);
	} else {
		avio_context_free(&io_context);
	}
	return success;
}

Ref<AudioStreamPlayback> ffmpeg_audio_playback_from_ogg_packets(const Ref<OggPacketSequence> &p_sequence) {
	ERR_FAIL_COND_V(p_sequence.is_null(), Ref<AudioStreamPlayback>());
	Ref<OggPacketSequencePlayback> sequence_playback = p_sequence->instantiate_playback();
	ERR_FAIL_COND_V(sequence_playback.is_null(), Ref<AudioStreamPlayback>());

	ogg_stream_state stream;
	ERR_FAIL_COND_V(ogg_stream_init(&stream, 1) != 0, Ref<AudioStreamPlayback>());
	Vector<uint8_t> ogg_data;
	ogg_packet *packet = nullptr;
	while (sequence_playback->next_ogg_packet(&packet)) {
		if (ogg_stream_packetin(&stream, packet) != 0) {
			ogg_stream_clear(&stream);
			return Ref<AudioStreamPlayback>();
		}
		ogg_page page;
		while (ogg_stream_pageout(&stream, &page) > 0) {
			int old_size = ogg_data.size();
			ogg_data.resize(old_size + page.header_len + page.body_len);
			memcpy(ogg_data.ptrw() + old_size, page.header, page.header_len);
			memcpy(ogg_data.ptrw() + old_size + page.header_len, page.body, page.body_len);
		}
	}
	ogg_page page;
	while (ogg_stream_flush(&stream, &page) > 0) {
		int old_size = ogg_data.size();
		ogg_data.resize(old_size + page.header_len + page.body_len);
		memcpy(ogg_data.ptrw() + old_size, page.header, page.header_len);
		memcpy(ogg_data.ptrw() + old_size + page.header_len, page.body, page.body_len);
	}
	ogg_stream_clear(&stream);
	return ffmpeg_audio_playback_from_buffer(ogg_data, nullptr);
}

double AudioStreamMedia::get_length() const {
	return 0.0;
}

void AudioStreamMedia::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_file", "file"), &AudioStreamMedia::set_file);
	ClassDB::bind_method(D_METHOD("get_file"), &AudioStreamMedia::get_file);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "file"), "set_file", "get_file");
}
