#include "video_stream_media.h"

#include "ffmpeg_file_io.h"
#include "core/io/file_access.h"
#include "core/math/math_funcs.h"
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
	int64_t base = 0;
	if (p_whence == SEEK_CUR) {
		base = io->position;
	} else if (p_whence == SEEK_END) {
		base = io->size;
	} else if (p_whence != SEEK_SET) {
		return AVERROR(EINVAL);
	}
	if (p_offset < -base || p_offset > io->size - base) {
		return AVERROR(EINVAL);
	}
	const int64_t position = base + p_offset;
	io->position = position;
	return position;
}

class AudioStreamPlaybackFFmpeg : public AudioStreamPlayback {
	GDCLASS(AudioStreamPlaybackFFmpeg, AudioStreamPlayback);

	FFmpegFileIO file_io;
	FFmpegBufferIO buffer_io;
	Vector<uint8_t> encoded;
	String path;
	AVFormatContext *format = nullptr;
	AVIOContext *io_context = nullptr;
	AVCodecContext *codec = nullptr;
	AVCodecParserContext *parser = nullptr;
	AVFrame *frame = nullptr;
	AVPacket *packet = nullptr;
	SwrContext *resampler = nullptr;
	int stream_index = -1;
	int output_sample_rate = 44100;
	bool raw_aac = false;
	bool packet_pending = false;
	bool draining = false;
	bool parser_flushed = false;
	bool eof = false;
	bool failed = false;
	bool playing = false;
	bool rewind_pending = false;
	int64_t raw_position = 0;
	int64_t encoded_size = 0;
	uint64_t frame_position = 0;
	uint64_t skip_frames = 0;
	uint64_t decoded_frames = 0;
	double length = 0.0;
	Vector<float> pcm;
	int pcm_position = 0;
	int pcm_frames = 0;
	Ref<AudioStream> loop_stream;
	bool looping_override = false;
	bool looping = false;
	int loops = 0;

	void close_decoder() {
		swr_free(&resampler);
		av_frame_free(&frame);
		av_packet_free(&packet);
		avcodec_free_context(&codec);
		if (parser) {
			av_parser_close(parser);
			parser = nullptr;
		}
		avformat_close_input(&format);
		if (io_context) {
			av_freep(&io_context->buffer);
			avio_context_free(&io_context);
		}
		file_io.close();
		pcm.clear();
		pcm_position = 0;
		pcm_frames = 0;
	}

	bool open_decoder() {
		close_decoder();
		packet_pending = false;
		draining = false;
		parser_flushed = false;
		eof = false;
		failed = false;
		raw_position = 0;
		decoded_frames = 0;
		const AVCodec *decoder = nullptr;
		if (raw_aac) {
			decoder = avcodec_find_decoder(AV_CODEC_ID_AAC);
			parser = av_parser_init(AV_CODEC_ID_AAC);
			if (!decoder || !parser) {
				return false;
			}
			codec = avcodec_alloc_context3(decoder);
			if (!codec) {
				return false;
			}
			codec->sample_rate = 48000;
			av_channel_layout_default(&codec->ch_layout, 2);
		} else {
			if (!path.is_empty()) {
				if (file_io.open(&format, path) < 0) {
					return false;
				}
			} else {
				buffer_io = { encoded.ptr(), encoded_size, 0 };
				uint8_t *buffer = static_cast<uint8_t *>(av_malloc(32768));
				if (!buffer) {
					return false;
				}
				io_context = avio_alloc_context(buffer, 32768, 0, &buffer_io, _ffmpeg_buffer_read, nullptr, _ffmpeg_buffer_seek);
				if (!io_context) {
					av_free(buffer);
					return false;
				}
				format = avformat_alloc_context();
				if (!format) {
					return false;
				}
				io_context->seekable = AVIO_SEEKABLE_NORMAL;
				format->pb = io_context;
				format->flags |= AVFMT_FLAG_CUSTOM_IO;
				if (avformat_open_input(&format, nullptr, nullptr, nullptr) < 0) {
					return false;
				}
			}
			if (avformat_find_stream_info(format, nullptr) < 0) {
				return false;
			}
			stream_index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
			if (stream_index < 0) {
				return false;
			}
			const AVStream *stream = format->streams[stream_index];
			decoder = avcodec_find_decoder(stream->codecpar->codec_id);
			if (!decoder) {
				return false;
			}
			codec = avcodec_alloc_context3(decoder);
			if (!codec || avcodec_parameters_to_context(codec, stream->codecpar) < 0) {
				return false;
			}
			if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
				length = stream->duration * av_q2d(stream->time_base);
			} else if (format->duration != AV_NOPTS_VALUE && format->duration > 0) {
				length = double(format->duration) / AV_TIME_BASE;
			}
		}
		codec->thread_count = 1;
		if (avcodec_open2(codec, decoder, nullptr) < 0) {
			return false;
		}
		packet = av_packet_alloc();
		frame = av_frame_alloc();
		return packet && frame;
	}

	int read_packet() {
		if (!raw_aac) {
			int result;
			do {
				av_packet_unref(packet);
				result = av_read_frame(format, packet);
			} while (result >= 0 && packet->stream_index != stream_index);
			return result;
		}
		while (raw_position < encoded_size || !parser_flushed) {
			const int amount = int(MIN(int64_t(4096), encoded_size - raw_position));
			uint8_t *data = nullptr;
			int size = 0;
			const int consumed = av_parser_parse2(parser, codec, &data, &size,
					amount ? encoded.ptr() + raw_position : nullptr, amount,
					AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
			if (consumed < 0 || (amount && !consumed && !size)) {
				return AVERROR_INVALIDDATA;
			}
			raw_position += consumed;
			if (!amount) {
				parser_flushed = true;
			}
			if (size) {
				av_packet_unref(packet);
				// Parser output refers to encoded or parser-owned memory until the next call.
				packet->data = data;
				packet->size = size;
				return 0;
			}
		}
		return AVERROR_EOF;
	}

	bool convert_frame(bool p_flush) {
		if (!resampler) {
			if (p_flush) {
				return false;
			}
			AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
			if (swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_FLT, output_sample_rate,
						&frame->ch_layout, static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr) < 0 ||
					!resampler || swr_init(resampler) < 0) {
				failed = true;
				return false;
			}
		}
		const int count = swr_get_out_samples(resampler, p_flush ? 0 : frame->nb_samples);
		// Bound malformed frames as well as normal decoder buffering (2 MiB of PCM).
		if (count < 0 || count > 262144 || pcm.resize(count * 2) != OK) {
			failed = true;
			return false;
		}
		uint8_t *output[] = { reinterpret_cast<uint8_t *>(pcm.ptrw()) };
		const int converted = swr_convert(resampler, output, count,
				p_flush ? nullptr : const_cast<const uint8_t **>(frame->extended_data), p_flush ? 0 : frame->nb_samples);
		av_frame_unref(frame);
		if (converted < 0) {
			failed = true;
			return false;
		}
		pcm_frames = converted;
		pcm_position = 0;
		decoded_frames += converted;
		return converted > 0;
	}

	bool decode_chunk() {
		pcm_frames = 0;
		pcm_position = 0;
		while (!failed && !eof) {
			const int result = avcodec_receive_frame(codec, frame);
			if (result >= 0) {
				if (convert_frame(false)) {
					return true;
				}
				continue;
			}
			if (result == AVERROR_EOF) {
				if (convert_frame(true)) {
					return true;
				}
				eof = true;
				length = double(decoded_frames) / output_sample_rate;
				return false;
			}
			if (result != AVERROR(EAGAIN) || draining) {
				failed = true;
				break;
			}
			int read_result = 0;
			if (!packet_pending) {
				read_result = read_packet();
				if (read_result < 0 && read_result != AVERROR_EOF) {
					failed = true;
					break;
				}
				packet_pending = read_result >= 0;
			}
			const int sent = avcodec_send_packet(codec, packet_pending ? packet : nullptr);
			if (sent < 0) {
				failed = true;
				break;
			}
			draining = !packet_pending;
			av_packet_unref(packet);
			packet_pending = false;
		}
		if (failed) {
			ERR_PRINT("FFmpeg: audio decoding failed.");
		}
		return false;
	}

	bool ensure_samples() {
		if (rewind_pending) {
			rewind_pending = false;
			if (!open_decoder()) {
				failed = true;
				return false;
			}
		}
		while (true) {
			if (pcm_position >= pcm_frames && !decode_chunk()) {
				return false;
			}
			const uint64_t skipped = MIN(skip_frames, uint64_t(pcm_frames - pcm_position));
			pcm_position += skipped;
			skip_frames -= skipped;
			if (pcm_position < pcm_frames) {
				return true;
			}
		}
	}

public:
	~AudioStreamPlaybackFFmpeg() override { close_decoder(); }

	bool open_buffer(const Vector<uint8_t> &p_data, const char *p_format_name, const String &p_path, bool p_store_samples = true) {
		encoded = p_data;
		encoded_size = p_data.size();
		path = p_path;
		raw_aac = p_format_name && strcmp(p_format_name, "aac") == 0;
		output_sample_rate = MAX(1, int(AudioServer::get_singleton()->get_mix_rate()));
		if (raw_aac) {
			if (encoded.resize(encoded_size + AV_INPUT_BUFFER_PADDING_SIZE) != OK) {
				return false;
			}
			memset(encoded.ptrw() + encoded_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
		}
		if (!open_decoder() || !decode_chunk()) {
			return false;
		}
		if (!p_store_samples) {
			while (decode_chunk()) {
			}
		}
		return !failed;
	}

	void set_loop_stream(const Ref<AudioStream> &p_stream) { loop_stream = p_stream; }
	double get_decoded_length() const { return length; }
	void start(double p_from_pos = 0.0) override {
		seek(p_from_pos);
		playing = true;
		loops = 0;
	}
	void stop() override { playing = false; }
	bool is_playing() const override { return playing; }
	int get_loop_count() const override { return loops; }
	double get_playback_position() const override { return double(frame_position) / output_sample_rate; }
	void seek(double p_time) override {
		if (!Math::is_finite(p_time)) {
			return;
		}
		const double time = CLAMP(p_time, 0.0, length > 0 ? length : double(INT32_MAX));
		const uint64_t target = uint64_t(time * output_sample_rate);
		if (target == frame_position && !eof && !failed) {
			return;
		}
		// Rebuild decoder delay/resampler state, then discard in bounded chunks.
		// This preserves exact sample positions even for timestamp-poor raw AAC.
		rewind_pending = true;
		skip_frames = target;
		frame_position = target;
	}
	void set_parameter(const StringName &p_name, const Variant &p_value) override {
		if (p_name == SNAME("looping")) {
			looping_override = p_value != Variant();
			looping = looping_override && bool(p_value);
		}
	}
	Variant get_parameter(const StringName &p_name) const override {
		return looping_override && p_name == SNAME("looping") ? Variant(looping) : Variant();
	}
	void tag_used_streams() override {
		if (loop_stream.is_valid()) {
			loop_stream->tag_used(get_playback_position());
		}
	}
	int mix(AudioFrame *p_buffer, float, int p_frames) override {
		int mixed = 0;
		bool restarted_without_output = false;
		while (playing && mixed < p_frames) {
			const bool use_loop = looping_override ? looping : loop_stream.is_valid() && loop_stream->has_loop();
			uint64_t loop_end = UINT64_MAX;
			if (use_loop && loop_stream.is_valid() && loop_stream->get_bpm() > 0 && loop_stream->get_beat_count() > 0) {
				loop_end = uint64_t(loop_stream->get_beat_count() * double(output_sample_rate) * 60.0 / loop_stream->get_bpm());
			}
			if (frame_position >= loop_end || !ensure_samples()) {
				if (!failed && use_loop && !restarted_without_output) {
					double offset = loop_stream.is_valid() ? double(loop_stream->get("loop_offset")) : 0.0;
					if (!Math::is_finite(offset) || offset < 0 || offset >= length || offset * output_sample_rate >= loop_end) {
						offset = 0.0;
					}
					seek(offset);
					loops++;
					restarted_without_output = true;
					continue;
				}
				playing = false;
				break;
			}
			const int count = int(MIN(uint64_t(MIN(p_frames - mixed, pcm_frames - pcm_position)), loop_end - frame_position));
			for (int i = 0; i < count; i++) {
				p_buffer[mixed++] = AudioFrame(pcm[pcm_position * 2], pcm[pcm_position * 2 + 1]);
				pcm_position++;
			}
			frame_position += count;
			restarted_without_output = false;
		}
		for (int i = mixed; i < p_frames; i++) {
			p_buffer[i] = AudioFrame(0, 0);
		}
		return mixed;
	}
};

void AudioStreamMedia::set_file(const String &p_file) {
	file = p_file;
	length = -1.0;
}

String AudioStreamMedia::get_file() const {
	return file;
}

Ref<AudioStreamPlayback> AudioStreamMedia::instantiate_playback() {
    Ref<AudioStreamPlaybackFFmpeg> playback;
    if (file.get_extension().to_lower() == "aac") {
        Vector<uint8_t> data = FileAccess::get_file_as_bytes(file);
        ERR_FAIL_COND_V(data.is_empty(), Ref<AudioStreamPlayback>());
        playback = ffmpeg_audio_playback_from_buffer(data, "aac");
    } else {
        playback = ffmpeg_audio_playback_from_buffer(Vector<uint8_t>(), nullptr, file);
    }
    if (playback.is_valid() && playback->get_decoded_length() > 0) {
        length = playback->get_decoded_length();
    }
    return playback;
}
Ref<AudioStreamPlayback> ffmpeg_audio_playback_from_buffer(const Vector<uint8_t> &p_data, const char *p_format, const String &p_path, const Ref<AudioStream> &p_loop_stream) {
	Ref<AudioStreamPlaybackFFmpeg> playback;
	playback.instantiate();
	if (!playback->open_buffer(p_data, p_format, p_path)) {
		return Ref<AudioStreamPlayback>();
	}
	playback->set_loop_stream(p_loop_stream);
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
	}
	// Custom AVIO remains owned by us, including when opening the input fails.
	av_freep(&io_context->buffer);
	avio_context_free(&io_context);
	return success;
}

Ref<AudioStreamPlayback> ffmpeg_audio_playback_from_ogg_packets(const Ref<OggPacketSequence> &p_sequence, const Ref<AudioStream> &p_loop_stream) {
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
	return ffmpeg_audio_playback_from_buffer(ogg_data, nullptr, String(), p_loop_stream);
}

double AudioStreamMedia::get_length() const {
	if (length >= 0.0) {
		return length;
	}
	length = 0.0;
	if (file.is_empty()) {
		return length;
	}
	// Use container metadata without decoding or retaining the whole file.
	// Raw ADTS uses the same parser-based path as playback instead of probing.
	if (file.get_extension().to_lower() != "aac") {
		FFmpegFileIO file_io;
		AVFormatContext *format = nullptr;
		if (file_io.open(&format, file) >= 0 && avformat_find_stream_info(format, nullptr) >= 0) {
			const int index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
			if (index >= 0) {
				const AVStream *stream = format->streams[index];
				if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
					length = stream->duration * av_q2d(stream->time_base);
				} else if (format->duration != AV_NOPTS_VALUE && format->duration > 0) {
					length = double(format->duration) / AV_TIME_BASE;
				}
			}
		}
		avformat_close_input(&format);
	}
	if (length <= 0.0) {
		Ref<AudioStreamPlaybackFFmpeg> playback;
		playback.instantiate();
		bool opened = false;
		// Count samples without retaining decoded PCM for a duration-only query.
		if (file.get_extension().to_lower() == "aac") {
			const Vector<uint8_t> data = FileAccess::get_file_as_bytes(file);
			if (!data.is_empty()) {
				opened = playback->open_buffer(data, "aac", String(), false);
			}
		} else {
			opened = playback->open_buffer(Vector<uint8_t>(), nullptr, file, false);
		}
		if (opened) {
			length = playback->get_decoded_length();
		}
	}
	return length;
}

void AudioStreamMedia::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_file", "file"), &AudioStreamMedia::set_file);
	ClassDB::bind_method(D_METHOD("get_file"), &AudioStreamMedia::get_file);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "file"), "set_file", "get_file");
}
