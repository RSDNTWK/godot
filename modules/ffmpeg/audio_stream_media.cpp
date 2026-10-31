#include "ffmpeg_file_io.h"
#include "ffmpeg_ogg_helpers.h"
#include "ffmpeg_timeline.h"
#include "video_stream_media.h"

#include "core/io/file_access.h"
#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "servers/audio/audio_server.h"

#include "modules/ogg/ogg_packet_sequence.h"

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
	FFmpegIOWorkBudget *seek_budget = nullptr;
};

static int _ffmpeg_buffer_read(void *p_opaque, uint8_t *p_buffer, int p_buffer_size) {
	FFmpegBufferIO *io = static_cast<FFmpegBufferIO *>(p_opaque);
	int64_t available = io->size - io->position;
	if (available <= 0) {
		return AVERROR_EOF;
	}
	int amount = MIN(int64_t(p_buffer_size), available);
	if (io->seek_budget) {
		amount = io->seek_budget->read_size(amount);
		if (amount <= 0) {
			return AVERROR_EXIT;
		}
	}
	memcpy(p_buffer, io->data + io->position, amount);
	io->position += amount;
	return amount;
}

static int64_t _ffmpeg_buffer_seek(void *p_opaque, int64_t p_offset, int p_whence) {
	FFmpegBufferIO *io = static_cast<FFmpegBufferIO *>(p_opaque);
	if (io->seek_budget && !io->seek_budget->step()) {
		return AVERROR_EXIT;
	}
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
	int resampler_input_rate = 0;
	int64_t resampler_input_frames = 0;
	int64_t resampler_output_frames = 0;
	bool resampler_padding = false;
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
	bool force_rewind = false;
	bool indexed_timestamp_pending = false;
	int64_t raw_position = 0;
	int64_t encoded_size = 0;
	int64_t frame_position = 0;
	int last_mix_frames = 0;
	int64_t origin_timestamp = AV_NOPTS_VALUE;
	uint64_t decoded_frames = 0;
	double length = 0.0;
	Vector<float> pcm;
	int pcm_position = 0;
	int pcm_frames = 0;
	Ref<AudioStream> loop_stream;
	bool looping_override = false;
	bool looping = false;
	int loops = 0;

	enum class DecodeResult { READY,
		PENDING,
		END,
		ERROR };
	struct WorkBudget {
		int64_t initial;
		int64_t remaining;
		int64_t io_bytes_remaining;
		explicit WorkBudget(int64_t p_work) : initial(p_work), remaining(p_work), io_bytes_remaining(p_work == INT64_MAX ? INT64_MAX : 32768) {}
		bool step() {
			if (remaining <= 0) {
				return false;
			}
			remaining--;
			return true;
		}
	};
	bool is_sample = false;
	Ref<AudioSamplePlayback> sample_playback;
	bool embedded_video = false;
	bool origin_initialized = false;
	bool timestamps_usable = false;
	double timeline_origin = 0.0;
	int64_t pcm_start = 0;
	int64_t decoded_cursor = 0;
	double fractional_position = 0.0;
	struct CachedSample {
		int64_t position = -1;
		AudioFrame value;
	} sample_cache[2];
#ifdef DEV_ENABLED
	int64_t last_work_used = 0;
	int64_t last_work_limit = 0;
	int decoder_open_count = 0;
	int indexed_seek_count = 0;
	int64_t last_seek_io_bytes = 0;
#endif

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
		resampler_input_rate = 0;
		resampler_input_frames = 0;
		resampler_output_frames = 0;
		resampler_padding = false;
	}

	bool open_decoder() {
#ifdef DEV_ENABLED
		decoder_open_count++;
#endif
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
				if (FFmpegFileIO::configure_local_input(format) < 0) {
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
			codec->pkt_timebase = stream->time_base;
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

	void clear_sample_cache() {
		sample_cache[0].position = -1;
		sample_cache[1].position = -1;
	}

	DecodeResult rewind_decoder(WorkBudget &r_budget) {
		indexed_timestamp_pending = false;
		if (raw_aac) {
			av_parser_close(parser);
			parser = av_parser_init(AV_CODEC_ID_AAC);
			if (!parser) {
				return DecodeResult::ERROR;
			}
			raw_position = 0;
		} else {
			FFmpegIOWorkBudget io_budget(r_budget.remaining, r_budget.io_bytes_remaining);
			struct SeekScope {
				FFmpegFileIO &file;
				FFmpegBufferIO &buffer;
				SeekScope(FFmpegFileIO &p_file, FFmpegBufferIO &p_buffer, FFmpegIOWorkBudget *p_budget) : file(p_file), buffer(p_buffer) {
					file.set_seek_budget(p_budget);
					buffer.seek_budget = p_budget;
				}
				~SeekScope() {
					file.set_seek_budget(nullptr);
					buffer.seek_budget = nullptr;
				}
			} scope(file_io, buffer_io, &io_budget);
			auto interrupted = [&]() {
				if (!io_budget.exhausted) {
					return false;
				}
				// EXIT breaks generic seek's EAGAIN retry loop. A subsequent seek
				// resumes from the accumulated index, with a fresh bounded allowance.
				if (format->pb) {
					format->pb->error = 0;
					format->pb->eof_reached = 0;
				}
				return true;
			};
			const AVStream *stream = format->streams[stream_index];
			const double target = double(frame_position) / output_sample_rate;
			const double preroll = MAX(1.0, double(MAX(stream->codecpar->seek_preroll, codec->delay)) / MAX(1, codec->sample_rate));
			int64_t timestamp = 0;
			const bool indexed = !force_rewind && timestamps_usable && target > preroll &&
					(!embedded_video || codec->sample_rate == output_sample_rate) &&
					ffmpeg_timestamp_for_position(target - preroll, timeline_origin, stream->time_base, timestamp);
			int seek_result = -1;
			if (indexed) {
				seek_result = av_seek_frame(format, stream_index, timestamp, AVSEEK_FLAG_BACKWARD);
				if (interrupted()) {
					return DecodeResult::PENDING;
				}
			}
			if (indexed && seek_result >= 0) {
				indexed_timestamp_pending = true;
#ifdef DEV_ENABLED
				indexed_seek_count++;
#endif
			} else {
				int64_t origin = stream->start_time;
				if (origin == AV_NOPTS_VALUE && !ffmpeg_timestamp_for_position(0.0, timeline_origin, stream->time_base, origin)) {
					return DecodeResult::ERROR;
				}
				seek_result = av_seek_frame(format, stream_index, origin, AVSEEK_FLAG_BACKWARD);
				if (interrupted()) {
					return DecodeResult::PENDING;
				}
				if (seek_result < 0) {
					return DecodeResult::ERROR;
				}
			}
		}
		avcodec_flush_buffers(codec);
		av_packet_unref(packet);
		av_frame_unref(frame);
		swr_free(&resampler);
		packet_pending = false;
		draining = false;
		parser_flushed = false;
		eof = false;
		failed = false;
		decoded_frames = 0;
		decoded_cursor = 0;
		pcm_start = 0;
		pcm_frames = 0;
		pcm_position = 0;
		resampler_input_rate = 0;
		resampler_input_frames = 0;
		resampler_output_frames = 0;
		resampler_padding = false;
		clear_sample_cache();
		return DecodeResult::READY;
	}
	bool indexed_phase_is_exact() const {
		if (frame->sample_rate == output_sample_rate) {
			return true;
		}
		const AVRational time_base = format->streams[stream_index]->time_base;
		if (embedded_video || origin_timestamp == AV_NOPTS_VALUE || frame->sample_rate <= 0 ||
				time_base.num <= 0 || time_base.den <= 0 ||
				int64_t(time_base.num) * frame->sample_rate > time_base.den) {
			return false;
		}
		const int64_t timestamp = frame->best_effort_timestamp;
		if ((origin_timestamp > 0 && timestamp < INT64_MIN + origin_timestamp) ||
				(origin_timestamp < 0 && timestamp > INT64_MAX + origin_timestamp)) {
			return false;
		}
		const int64_t delta = timestamp - origin_timestamp;
		const AVRational sample_base = { 1, frame->sample_rate };
		const int64_t samples = av_rescale_q(delta, time_base, sample_base);
		return samples >= 0 && av_compare_ts(delta, time_base, samples, sample_base) == 0 &&
				((samples % frame->sample_rate) * output_sample_rate) % frame->sample_rate == 0;
	}

	int read_packet(WorkBudget &r_budget) {
		if (!raw_aac) {
			while (r_budget.step()) {
				av_packet_unref(packet);
				const int result = av_read_frame(format, packet);
				if (result < 0 || packet->stream_index == stream_index) {
					return result;
				}
			}
			return AVERROR(EAGAIN);
		}
		while (raw_position < encoded_size || !parser_flushed) {
			if (!r_budget.step()) {
				return AVERROR(EAGAIN);
			}
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
				packet->data = data;
				packet->size = size;
				return 0;
			}
		}
		return AVERROR_EOF;
	}

	bool convert_frame(bool p_flush) {
		int64_t start = decoded_cursor;
		if (!p_flush) {
			if (!origin_initialized) {
				timestamps_usable = !raw_aac && frame->best_effort_timestamp != AV_NOPTS_VALUE;
				timeline_origin = timestamps_usable ? frame->best_effort_timestamp * av_q2d(format->streams[stream_index]->time_base) : 0.0;
				origin_timestamp = timestamps_usable ? frame->best_effort_timestamp : AV_NOPTS_VALUE;
				origin_initialized = true;
			}
			if (!raw_aac && frame->best_effort_timestamp != AV_NOPTS_VALUE && (timestamps_usable || embedded_video)) {
				const AVRational time_base = format->streams[stream_index]->time_base;
				double stamp = (frame->best_effort_timestamp * av_q2d(time_base) - timeline_origin) * output_sample_rate;
				if (resampler) {
					stamp -= double(swr_get_delay(resampler, output_sample_rate));
				}
				if (!Math::is_finite(stamp) || stamp <= double(INT64_MIN) / 2 || stamp >= double(INT64_MAX) / 2) {
					failed = true;
					return false;
				}
				const int64_t candidate = int64_t(Math::round(stamp));
				const double tolerance = MAX(1.0, Math::ceil(av_q2d(time_base) * output_sample_rate));
				// Keep continuous samples across timestamp quantization, while preserving real gaps.
				if (!resampler || Math::abs(double(candidate) - double(decoded_cursor)) > tolerance) {
					start = candidate;
				}
			}
		}
		if (!resampler) {
			if (p_flush) {
				return false;
			}
			resampler_input_rate = frame->sample_rate;
			if (resampler_input_rate <= 0) {
				failed = true;
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
		if (!p_flush) {
			if (frame->nb_samples < 0 || resampler_input_frames > INT64_MAX - frame->nb_samples) {
				failed = true;
				return false;
			}
			resampler_input_frames += frame->nb_samples;
		}
		int64_t tail = 0;
		if (p_flush) {
			const int64_t expected = av_rescale_rnd(resampler_input_frames, output_sample_rate, resampler_input_rate, AV_ROUND_UP);
			if (expected < resampler_output_frames) {
				failed = true;
				return false;
			}
			tail = expected - resampler_output_frames;
			if (tail == 0) {
				return false;
			}
		}
		auto convert = [&]() {
			const int upper = swr_get_out_samples(resampler, p_flush ? 0 : frame->nb_samples);
			if (upper < 0) {
				return upper;
			}
			const int count = p_flush ? int(MIN(int64_t(upper), tail)) : upper;
			if (count > 262144 || pcm.resize(count * 2) != OK) {
				return AVERROR(ENOMEM);
			}
			uint8_t *output[] = { reinterpret_cast<uint8_t *>(pcm.ptrw()) };
			return swr_convert(resampler, output, count,
					p_flush ? nullptr : const_cast<const uint8_t **>(frame->extended_data), p_flush ? 0 : frame->nb_samples);
		};
		int converted = convert();
		if (p_flush && converted == 0 && tail > 0) {
			if (resampler_padding) {
				converted = AVERROR_INVALIDDATA;
			} else {
				// Very short inputs may not prime SWR's initial filter even after its
				// normal reflection drain. Complete that filter, retaining only the
				// real input duration; never publish the artificial padding.
				const int padding = int(MIN(262144.0, Math::ceil(MAX(64.0, 64.0 * resampler_input_rate / output_sample_rate))));
				converted = swr_inject_silence(resampler, padding);
				if (converted >= 0) {
					resampler_padding = true;
					converted = convert();
					if (converted == 0) {
						converted = AVERROR_INVALIDDATA;
					}
				}
			}
		}
		av_frame_unref(frame);
		if (converted < 0) {
			failed = true;
			return false;
		}
		pcm_start = start;
		pcm_frames = converted;
		pcm_position = 0;
		decoded_frames += converted;
		resampler_output_frames += converted;
		if (start > INT64_MAX - converted) {
			failed = true;
			return false;
		}
		decoded_cursor = start + converted;
		return converted > 0;
	}

	DecodeResult decode_chunk(WorkBudget &r_budget) {
		pcm_frames = 0;
		pcm_position = 0;
		while (!failed && !eof) {
			if (!r_budget.step()) {
				return DecodeResult::PENDING;
			}
			const int result = avcodec_receive_frame(codec, frame);
			if (result >= 0) {
				if (indexed_timestamp_pending) {
					indexed_timestamp_pending = false;
					const double first = frame->best_effort_timestamp == AV_NOPTS_VALUE ? 0.0 : frame->best_effort_timestamp * av_q2d(format->streams[stream_index]->time_base) - timeline_origin;
					if (frame->best_effort_timestamp == AV_NOPTS_VALUE || !Math::is_finite(first) || first * output_sample_rate > double(frame_position) || !indexed_phase_is_exact()) {
						// An unusable/forward-only seek cannot silently discard the target.
						force_rewind = true;
						rewind_pending = true;
						av_frame_unref(frame);
						return DecodeResult::PENDING;
					}
				}
				if (convert_frame(false)) {
					return DecodeResult::READY;
				}
				continue;
			}
			if (result == AVERROR_EOF) {
				if (convert_frame(true)) {
					return DecodeResult::READY;
				}
				if (failed) {
					break;
				}
				eof = true;
				length = double(MAX(int64_t(0), decoded_cursor)) / output_sample_rate;
				return DecodeResult::END;
			}
			if (result != AVERROR(EAGAIN) || draining) {
				failed = true;
				break;
			}
			if (!packet_pending) {
				const int read_result = read_packet(r_budget);
				if (read_result == AVERROR(EAGAIN)) {
					return DecodeResult::PENDING;
				}
				if (read_result < 0 && read_result != AVERROR_EOF) {
					failed = true;
					break;
				}
				packet_pending = read_result >= 0;
			}
			if (!r_budget.step()) {
				return DecodeResult::PENDING;
			}
			const int sent = avcodec_send_packet(codec, packet_pending ? packet : nullptr);
			if (sent == AVERROR(EAGAIN)) {
				return DecodeResult::PENDING;
			}
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
			return DecodeResult::ERROR;
		}
		return DecodeResult::END;
	}

	DecodeResult sample_at(int64_t p_position, AudioFrame &r_sample, WorkBudget &r_budget) {
		CachedSample &cached = sample_cache[p_position & 1];
		if (cached.position == p_position) {
			r_sample = cached.value;
			return DecodeResult::READY;
		}
		if (rewind_pending) {
			if (!r_budget.step()) {
				return DecodeResult::PENDING;
			}
			const DecodeResult rewound = rewind_decoder(r_budget);
			if (rewound != DecodeResult::READY) {
				failed = rewound == DecodeResult::ERROR;
				return rewound;
			}
			rewind_pending = false;
		}
		while (!failed) {
			if (pcm_frames > 0 && p_position < pcm_start) {
				r_sample = AudioFrame(0, 0);
				break;
			}
			if (pcm_frames > 0 && p_position < pcm_start + pcm_frames) {
				const int index = int(p_position - pcm_start);
				r_sample = AudioFrame(pcm[index * 2], pcm[index * 2 + 1]);
				break;
			}
			const DecodeResult result = decode_chunk(r_budget);
			if (result != DecodeResult::READY) {
				return result;
			}
		}
		if (failed) {
			return DecodeResult::ERROR;
		}
		cached.position = p_position;
		cached.value = r_sample;
		return DecodeResult::READY;
	}

public:
	~AudioStreamPlaybackFFmpeg() override { close_decoder(); }

	bool open_buffer(const Vector<uint8_t> &p_data, const char *p_format_name, const String &p_path, bool p_store_samples = true, bool p_embedded_video = false, double p_timeline_origin = 0.0) {
		encoded = p_data;
		encoded_size = p_data.size();
		path = p_path;
		raw_aac = p_format_name && strcmp(p_format_name, "aac") == 0;
		embedded_video = p_embedded_video;
		origin_initialized = embedded_video;
		timestamps_usable = embedded_video;
		timeline_origin = p_timeline_origin;
		output_sample_rate = MAX(1, int(AudioServer::get_singleton()->get_mix_rate()));
		if (raw_aac) {
			if (encoded_size > INT64_MAX - AV_INPUT_BUFFER_PADDING_SIZE || encoded.resize(encoded_size + AV_INPUT_BUFFER_PADDING_SIZE) != OK) {
				return false;
			}
			memset(encoded.ptrw() + encoded_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
		}
		if (!open_decoder()) {
			return false;
		}
		WorkBudget initial(4096);
		DecodeResult result = decode_chunk(initial);
		if (!p_store_samples) {
			// Duration queries run outside mix and intentionally count the entire stream.
			while (result == DecodeResult::READY || result == DecodeResult::PENDING) {
				WorkBudget work(4096);
				result = decode_chunk(work);
			}
		}
		return !failed && (result != DecodeResult::END || decoded_frames > 0);
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
	double get_playback_position() const override { return (double(frame_position) + fractional_position) / output_sample_rate; }

	void seek(double p_time) override {
		if (!Math::is_finite(p_time)) {
			return;
		}
		const double maximum = MIN(length > 0 ? length : double(INT32_MAX), double(INT64_MAX) / 4 / output_sample_rate);
		const double time = CLAMP(p_time, 0.0, maximum);
		const int64_t target = int64_t(time * output_sample_rate);
		if (target == frame_position && fractional_position == 0.0 && !eof && !failed) {
			return;
		}
		// Reuse the opened demuxer/codec. All replay/discard loops share the mix work budget.
		rewind_pending = true;
		force_rewind = false;
		frame_position = target;
		fractional_position = 0.0;
		clear_sample_cache();
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
	void set_is_sample(bool p_is_sample) override { is_sample = p_is_sample; }
	bool get_is_sample() const override { return is_sample; }
	Ref<AudioSamplePlayback> get_sample_playback() const override { return sample_playback; }
	void set_sample_playback(const Ref<AudioSamplePlayback> &p_playback) override {
		sample_playback = p_playback;
		if (sample_playback.is_valid()) {
			sample_playback->stream_playback = Ref<AudioStreamPlayback>(this);
		}
	}
#ifdef DEV_ENABLED
	FFmpegAudioWorkStats get_work_stats() const { return { last_work_used, last_work_limit, decoder_open_count, indexed_seek_count, last_seek_io_bytes }; }
#endif

	int get_last_mix_frames() const { return last_mix_frames; }
	int mix(AudioFrame *p_buffer, float p_rate_scale, int p_frames) override {
		last_mix_frames = 0;
		ERR_FAIL_COND_V(p_frames < 0 || (p_frames > 0 && p_buffer == nullptr), 0);
		const double target_rate = AudioServer::get_singleton()->get_mix_rate();
		const double speed = embedded_video ? 1.0 : double(p_rate_scale) * AudioServer::get_singleton()->get_playback_speed_scale();
		const double rate = speed * output_sample_rate / target_rate;
		if (!Math::is_finite(rate) || rate <= 0.0 || rate >= double(INT64_MAX) / 4) {
			failed = true;
			playing = false;
		}
		// Sample backends skip live mixing. Their private decoder is explicitly marked
		// as sample playback when rendering the entire offline sample in one call.
		WorkBudget work(is_sample ? INT64_MAX : 64 + ((int64_t(p_frames) + 127) / 128) * 8);
		int mixed = 0;
		bool pending = false;
		bool restarted_without_output = false;
		while (playing && mixed < p_frames) {
			const bool use_loop = looping_override ? looping : loop_stream.is_valid() && loop_stream->has_loop();
			int64_t loop_end = INT64_MAX;
			if (use_loop && loop_stream.is_valid() && loop_stream->get_bpm() > 0 && loop_stream->get_beat_count() > 0) {
				const double end = loop_stream->get_beat_count() * double(output_sample_rate) * 60.0 / loop_stream->get_bpm();
				if (Math::is_finite(end) && end >= 0.0 && end < double(INT64_MAX)) {
					loop_end = int64_t(end);
				}
			}
			AudioFrame current;
			DecodeResult result = frame_position >= loop_end ? DecodeResult::END : sample_at(frame_position, current, work);
			if (result == DecodeResult::PENDING) {
				pending = true;
				break;
			}
			if (result != DecodeResult::READY) {
				if (result == DecodeResult::END && !failed && use_loop && !restarted_without_output) {
					if (!work.step()) {
						pending = true;
						break;
					}
					double offset = loop_stream.is_valid() ? double(loop_stream->get("loop_offset")) : 0.0;
					if (!Math::is_finite(offset) || offset < 0 || offset >= length || offset * output_sample_rate >= loop_end) {
						offset = 0.0;
					}
					const int64_t end = MIN(loop_end, decoded_cursor);
					const int64_t begin = int64_t(offset * output_sample_rate);
					const double overshoot = MAX(0.0, double(frame_position - end) + fractional_position);
					const double span = double(end - begin);
					seek(offset);
					if (span > 0.0) {
						const double remainder = Math::fmod(overshoot, span);
						frame_position += int64_t(remainder);
						fractional_position = remainder - Math::floor(remainder);
						const double crossed = 1.0 + Math::floor(overshoot / span);
						loops += int(MIN(crossed, double(INT_MAX - loops)));
					} else {
						loops += loops < INT_MAX ? 1 : 0;
					}
					restarted_without_output = true;
					continue;
				}
				playing = false;
				break;
			}
			AudioFrame value = current;
			if (fractional_position > 0.0) {
				AudioFrame next(0, 0);
				result = sample_at(frame_position + 1, next, work);
				if (result == DecodeResult::PENDING) {
					pending = true;
					break;
				}
				if (result == DecodeResult::ERROR) {
					playing = false;
					break;
				}
				value = current + (next - current) * float(fractional_position);
			}
			p_buffer[mixed++] = value;
			const double advance = fractional_position + rate;
			const int64_t whole = int64_t(Math::floor(advance));
			if (frame_position > INT64_MAX / 2 - whole) {
				failed = true;
				playing = false;
				break;
			}
			frame_position += whole;
			fractional_position = advance - double(whole);
			restarted_without_output = false;
		}
		for (int i = mixed; i < p_frames; i++) {
			p_buffer[i] = AudioFrame(0, 0);
		}
#ifdef DEV_ENABLED
		last_work_used = work.initial - work.remaining;
		last_work_limit = work.initial;
		last_seek_io_bytes = (work.initial == INT64_MAX ? INT64_MAX : 32768) - work.io_bytes_remaining;
#endif
		// AudioServer treats a short return as terminal. A work/EAGAIN yield is temporary.
		last_mix_frames = mixed;
		return pending ? p_frames : mixed;
	}
};

int ffmpeg_audio_mix_for_video(const Ref<AudioStreamPlayback> &p_playback, AudioFrame *p_buffer, int p_frames) {
	AudioStreamPlaybackFFmpeg *playback = Object::cast_to<AudioStreamPlaybackFFmpeg>(p_playback.ptr());
	ERR_FAIL_NULL_V(playback, 0);
	playback->mix(p_buffer, 1.0, p_frames);
	return playback->get_last_mix_frames();
}

#ifdef DEV_ENABLED
bool ffmpeg_audio_get_work_stats(const Ref<AudioStreamPlayback> &p_playback, FFmpegAudioWorkStats &r_stats) {
	const AudioStreamPlaybackFFmpeg *playback = Object::cast_to<AudioStreamPlaybackFFmpeg>(p_playback.ptr());
	if (!playback) {
		return false;
	}
	r_stats = playback->get_work_stats();
	return true;
}
#endif

void AudioStreamMedia::set_file(const String &p_file) {
	file = p_file;
	length = -1.0;
}

String AudioStreamMedia::get_file() const {
	return file;
}

Ref<AudioStreamPlayback> AudioStreamMedia::instantiate_playback() {
	// Raw ADTS reads through FileAccess before creating a decoder.
	if (FFmpegFileIO::is_remote_path(file)) {
		return Ref<AudioStreamPlayback>();
	}
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
Ref<AudioStreamPlayback> ffmpeg_audio_playback_from_buffer(const Vector<uint8_t> &p_data, const char *p_format, const String &p_path, const Ref<AudioStream> &p_loop_stream, bool p_embedded_video, double p_timeline_origin) {
	Ref<AudioStreamPlaybackFFmpeg> playback;
	playback.instantiate();
	if (!playback->open_buffer(p_data, p_format, p_path, true, p_embedded_video, p_timeline_origin)) {
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
	if (FFmpegFileIO::configure_local_input(format) < 0) {
		avformat_free_context(format);
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
		// Keep the sequence's known page boundaries. Greedy pageout can mix
		// headers with audio and publish completed packets with no granule,
		// losing the decoder's initial/final sample trimming information.
		if (packet->granulepos >= 0) {
			ogg_page page;
			while (ogg_stream_flush_fill(&stream, &page, 65025) > 0) {
				if (ffmpeg_append_ogg_page(ogg_data, page) != OK) {
					ogg_stream_clear(&stream);
					return Ref<AudioStreamPlayback>();
				}
			}
		}
	}
	ogg_page page;
	while (ogg_stream_flush_fill(&stream, &page, 65025) > 0) {
		if (ffmpeg_append_ogg_page(ogg_data, page) != OK) {
			ogg_stream_clear(&stream);
			return Ref<AudioStreamPlayback>();
		}
	}
	ogg_stream_clear(&stream);
	return ffmpeg_audio_playback_from_buffer(ogg_data, nullptr, String(), p_loop_stream);
}

double AudioStreamMedia::get_length() const {
	if (length >= 0.0) {
		return length;
	}
	length = 0.0;
	if (file.is_empty() || FFmpegFileIO::is_remote_path(file)) {
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
