#include "ffmpeg_file_io.h"
#include "ffmpeg_media_metadata.h"
#include "video_stream_media.h"
#ifdef MODULE_MEDIA_STREAM_ENABLED
#include "audio_stream_pcm_decoder.h"
#include "ffmpeg_async_playback.h"
#include "ffmpeg_packet_reader.h"
#endif
#include "core/io/file_access.h"
#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
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
#ifdef MODULE_MEDIA_STREAM_ENABLED
	FFmpegPacketReader packet_reader;
#endif
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
	String decode_error;
	bool playing = false;
	bool rewind_pending = false;
	bool packet_waiting = false;
	int64_t raw_position = 0;
	int64_t encoded_size = 0;
	uint64_t frame_position = 0;
	uint64_t skip_frames = 0;
	uint64_t decoded_frames = 0;
	double length = 0.0;
	Dictionary stream_metadata;
	Vector<float> pcm;
	int pcm_position = 0;
	int pcm_frames = 0;
	Ref<AudioStream> loop_stream;
	bool looping_override = false;
	bool looping = false;
	int loops = 0;
	double remote_seek_target = -1.0;
	const SafeFlag *remote_cancel = nullptr;
	double remote_origin = 0.0;
	uint64_t silence_frames = 0;
	uint64_t live_preroll_deadline = 0;
	AVRational source_time_base = { 0, 1 };
	int64_t source_start_time = AV_NOPTS_VALUE;
	double input_origin = 0.0;
	bool input_origin_known = false;
	bool remote_live_hls = false;
	bool hls_pdt_timeline = false;
	bool finite_pcm_preparation = false;
	uint64_t timeline_trimmed_frames = 0;
	uint64_t timeline_padded_frames = 0;
	bool read_ahead_used = false;
	double read_ahead_prefill_seconds = 6.0;

	void close_decoder() {
#ifdef MODULE_MEDIA_STREAM_ENABLED
		// The reader owns all format reads until it has joined. Its stop flag
		// also interrupts active Godot HTTP requests, including HLS children.
		packet_reader.stop();
#endif
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
#ifdef MODULE_MEDIA_STREAM_ENABLED
		packet_reader.reset();
#endif
		packet_pending = false;
		draining = false;
		parser_flushed = false;
		eof = false;
		failed = false;
		decode_error = String();
		packet_waiting = false;
		source_time_base = { 0, 1 };
		source_start_time = AV_NOPTS_VALUE;
		input_origin = 0.0;
		input_origin_known = false;
		remote_live_hls = false;
		hls_pdt_timeline = false;
		timeline_trimmed_frames = timeline_padded_frames = 0;
		read_ahead_used = false;
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
#ifdef MODULE_MEDIA_STREAM_ENABLED
			if (remote_cancel && format->iformat && strcmp(format->iformat->name, "hls") == 0) {
				for (unsigned int i = 0; i < format->nb_streams; i++) {
					format->streams[i]->discard = int(i) == stream_index ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
				}
			}
#endif
			const AVStream *stream = format->streams[stream_index];
			const AVDictionaryEntry *timeline = av_dict_get(stream->metadata, "toi_hls_pdt_timeline", nullptr, 0);
			hls_pdt_timeline = remote_cancel && !finite_pcm_preparation && format->iformat &&
					strcmp(format->iformat->name, "hls") == 0 && timeline && strcmp(timeline->value, "1") == 0;
			source_time_base = stream->time_base;
			source_start_time = stream->start_time;
#ifdef MODULE_MEDIA_STREAM_ENABLED
			if (remote_cancel) {
				remote_live_hls = format->iformat && strcmp(format->iformat->name, "hls") == 0 && format->duration <= 0;
				input_origin_known = format->start_time != AV_NOPTS_VALUE;
				input_origin = input_origin_known ? double(format->start_time) / AV_TIME_BASE : 0.0;
				remote_origin = format->start_time == AV_NOPTS_VALUE ? 0.0 : double(format->start_time) / AV_TIME_BASE;
				double audio_start = stream->start_time == AV_NOPTS_VALUE ? remote_origin : stream->start_time * av_q2d(stream->time_base);
				silence_frames = uint64_t(CLAMP(audio_start - remote_origin, 0.0, double(INT32_MAX)) * output_sample_rate);
			}
#endif
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
#ifdef MODULE_MEDIA_STREAM_ENABLED
			if (remote_cancel && format->duration != AV_NOPTS_VALUE && format->duration > 0) {
				length = MAX(length, double(format->duration) / AV_TIME_BASE);
			}
#endif
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
#ifdef MODULE_MEDIA_STREAM_ENABLED
			if (packet_reader.is_started()) {
				av_packet_unref(packet);
				return packet_reader.read(packet);
			}
#endif
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
		packet_waiting = false;
		while (!failed && !eof) {
			if (live_preroll_deadline && ((remote_cancel && remote_cancel->is_set()) || OS::get_singleton()->get_ticks_usec() >= live_preroll_deadline)) {
				failed = true;
				return false;
			}
			const int result = avcodec_receive_frame(codec, frame);
			if (result >= 0) {
				if (hls_pdt_timeline) {
					// Declared HLS epochs use one UTC timeline for separate inputs.
					// Reconcile encoder overlap/gaps against consumed PCM, never
					// concatenate both epochs and silently accumulate A/V drift.
					if (frame->best_effort_timestamp == AV_NOPTS_VALUE || frame->sample_rate <= 0) {
						decode_error = "The dated HLS audio frame has no usable timestamp.";
						failed = true;
						return false;
					}
					const double delay = resampler ? double(swr_get_delay(resampler, frame->sample_rate)) / frame->sample_rate : 0.0;
					const double pts = frame->best_effort_timestamp * av_q2d(source_time_base) - remote_origin - delay;
					const double difference = pts - double(frame_position) / output_sample_rate;
					if (!Math::is_finite(difference) || Math::abs(difference) > 30.0) {
						decode_error = "The HLS audio timestamp changed without a usable declared timeline.";
						failed = true;
						return false;
					}
					const int64_t delta = int64_t(Math::round(difference * output_sample_rate));
					// One sample of rescaling error must not become repeated padding.
					if (delta < -1) {
						skip_frames = uint64_t(-delta);
						timeline_trimmed_frames += MIN(skip_frames, uint64_t(frame->nb_samples) * output_sample_rate / frame->sample_rate);
					} else if (delta > 1) {
						silence_frames = uint64_t(delta);
						timeline_padded_frames += silence_frames;
					}
					remote_seek_target = -1.0;
				} else if (remote_seek_target >= 0.0 && format && stream_index >= 0) {
					if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
						double pts = frame->best_effort_timestamp * av_q2d(source_time_base) - remote_origin;
						if (remote_seek_target - pts > 30.0) {
							failed = true;
							return false;
						}
						if (live_preroll_deadline) {
							if (Math::abs(remote_seek_target - pts) > 30.0) {
								failed = true;
								return false;
							}
							silence_frames = uint64_t(MAX(pts - remote_seek_target, 0.0) * output_sample_rate);
						}
						skip_frames = uint64_t(CLAMP(remote_seek_target - pts, 0.0, 30.0) * output_sample_rate);
					} else if (live_preroll_deadline) {
						failed = true;
						return false;
					}
					remote_seek_target = -1.0;
				}
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
#ifdef MODULE_MEDIA_STREAM_ENABLED
				if (!remote_cancel) {
					length = double(decoded_frames) / output_sample_rate;
				}
#else
				length = double(decoded_frames) / output_sample_rate;
#endif
				return false;
			}
			if (result != AVERROR(EAGAIN) || draining) {
				decode_error = vformat("Audio decoder receive failed (FFmpeg error %d, draining %s).", result, draining ? "yes" : "no");
				failed = true;
				break;
			}
			int read_result = 0;
			if (!packet_pending) {
				read_result = read_packet();
				if (read_result == AVERROR(EAGAIN)) {
					// A live packet producer may still be fetching its next segment.
					// Preserve decoder state and report temporary PCM starvation.
					packet_waiting = true;
					return false;
				}
				if (read_result < 0 && read_result != AVERROR_EOF) {
					decode_error = vformat("Audio packet input failed (FFmpeg error %d).", read_result);
					failed = true;
					break;
				}
				packet_pending = read_result >= 0;
			}
			const int sent = avcodec_send_packet(codec, packet_pending ? packet : nullptr);
			if (sent == AVERROR(EAGAIN)) {
				packet_waiting = true;
				return false;
			}
			if (sent < 0) {
				decode_error = vformat("Audio decoder send failed (FFmpeg error %d).", sent);
				failed = true;
				break;
			}
			draining = !packet_pending;
			av_packet_unref(packet);
			packet_pending = false;
		}
		if (failed && !(remote_cancel && remote_cancel->is_set())) {
			ERR_PRINT(decode_error.is_empty() ? String("FFmpeg: audio decoding failed.") : decode_error);
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

#ifdef MODULE_MEDIA_STREAM_ENABLED
	bool prepare_live_origin(double p_origin) {
		if (!remote_live_hls || !input_origin_known || read_ahead_used || playing || frame_position != 0 || !Math::is_finite(p_origin)) {
			return false;
		}
		const double discard_seconds = p_origin - remote_origin;
		if (discard_seconds < 0.0 || discard_seconds > 30.0) {
			return false;
		}
		remote_origin = p_origin;
		if (discard_seconds == 0.0) {
			return true;
		}
		// open_buffer() has already decoded the first PCM block. Discard it,
		// then trim by source PTS without seeking/reopening the live input.
		swr_free(&resampler);
		pcm_position = pcm_frames = 0;
		skip_frames = 0;
		silence_frames = 0;
		remote_seek_target = 0.0;
		live_preroll_deadline = OS::get_singleton()->get_ticks_usec() + 30000000;
		const bool prepared = ensure_samples();
		live_preroll_deadline = 0;
		return prepared && !failed && !eof;
	}
#endif

public:
	~AudioStreamPlaybackFFmpeg() override { close_decoder(); }
	void set_remote_options(const Dictionary &p_headers, const SafeFlag *p_cancel, FFmpegDVRWindow *p_dvr = nullptr, bool p_primary = true, bool p_allow_http = false) {
		remote_cancel = p_cancel;
#ifdef MODULE_MEDIA_STREAM_ENABLED
		file_io.set_remote_options(p_headers, p_cancel, packet_reader.get_stop_flag());
		file_io.set_allow_http(p_allow_http);
		file_io.set_dvr(Ref<FFmpegDVRWindow>(p_dvr), p_primary);
#else
		file_io.set_remote_options(p_headers, p_cancel);
#endif
	}

	bool open_buffer(const Vector<uint8_t> &p_data, const char *p_format_name, const String &p_path, bool p_store_samples = true) {
		finite_pcm_preparation = false;
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
		stream_metadata = FFmpegMediaMetadata::container(format, stream_index, path, length, remote_live_hls);
		return !failed;
	}

#ifdef MODULE_MEDIA_STREAM_ENABLED
	bool open_pcm_source(const String &p_path, const Dictionary &p_headers, const SafeFlag *p_cancel, int p_rate, uint64_t p_deadline, bool p_allow_http) {
		finite_pcm_preparation = true;
		path = p_path;
		output_sample_rate = p_rate;
		remote_cancel = p_cancel;
		live_preroll_deadline = p_deadline;
		file_io.set_remote_options(p_headers, p_cancel, nullptr, p_deadline);
		file_io.set_allow_http(p_allow_http);
		if (!open_decoder()) {
			return false;
		}
		// This finite preparation path does not align playback clocks or pad gaps.
		// Only actual decoder samples belong in a sample-indexed loop buffer.
		silence_frames = 0;
		if (remote_live_hls) {
			decode_error = "PCM preparation requires finite audio, not a live HLS input.";
			failed = true;
			return false;
		}
		stream_metadata = FFmpegMediaMetadata::container(format, stream_index, path, length, false);
		return true;
	}

	int read_pcm_samples(AudioFrame *p_buffer, int p_frames) {
		int written = 0;
		while (written < p_frames) {
			if ((remote_cancel && remote_cancel->is_set()) || OS::get_singleton()->get_ticks_usec() >= live_preroll_deadline) {
				failed = true;
				break;
			}
			if (!ensure_samples()) {
				if (packet_waiting && !failed && !eof) {
					OS::get_singleton()->delay_usec(1000);
					continue;
				}
				break;
			}
			const int count = MIN(p_frames - written, pcm_frames - pcm_position);
			for (int i = 0; i < count; i++) {
				p_buffer[written++] = AudioFrame(pcm[pcm_position * 2], pcm[pcm_position * 2 + 1]);
				pcm_position++;
			}
		}
		// No tail fill, clock padding, real-time mixer call or starvation silence.
		return written;
	}
#endif

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
#ifdef MODULE_MEDIA_STREAM_ENABLED
		if (remote_cancel && format && stream_index >= 0) {
			if (read_ahead_used) {
				// A live format may have an interrupted/queued read in flight.
				// Public asynchronous seeks reopen a new generation instead.
				failed = true;
				return;
			}
			if (format->pb) {
				format->pb->error = 0;
				format->pb->eof_reached = 0;
			}
			if (av_seek_frame(format, stream_index, int64_t((time + remote_origin) / av_q2d(source_time_base)), AVSEEK_FLAG_BACKWARD) < 0) {
				failed = true;
				return;
			}
			avcodec_flush_buffers(codec);
			swr_free(&resampler);
			av_packet_unref(packet);
			av_frame_unref(frame);
			pcm_position = pcm_frames = 0;
			packet_pending = draining = eof = failed = rewind_pending = false;
			skip_frames = 0;
			remote_seek_target = time;
			double audio_start = source_start_time == AV_NOPTS_VALUE ? remote_origin : source_start_time * av_q2d(source_time_base);
			silence_frames = uint64_t(CLAMP(audio_start - remote_origin - time, 0.0, double(INT32_MAX)) * output_sample_rate);
			frame_position = target;
			packet_waiting = false;
			// A backward container seek may land well before the requested PCM.
			// Resolve decoder/resampler preroll here on the worker, before packet
			// read-ahead and public readiness. Otherwise prefilling packets before
			// the target can report completion with no playable audio yet.
			const uint64_t previous_deadline = live_preroll_deadline;
			const uint64_t seek_deadline = OS::get_singleton()->get_ticks_usec() + 30000000;
			live_preroll_deadline = previous_deadline ? MIN(previous_deadline, seek_deadline) : seek_deadline;
			const bool prepared = ensure_samples();
			live_preroll_deadline = previous_deadline;
			if (!prepared && !eof) {
				failed = true;
				if (decode_error.is_empty()) {
					decode_error = remote_cancel->is_set() ? "Audio seek was cancelled." : "Could not prepare audio at the requested seek position.";
				}
			}
			return;
		}
#endif
		// Rebuild decoder delay/resampler state, then discard in bounded chunks.
		// This preserves exact sample positions even for timestamp-poor raw AAC.
		rewind_pending = true;
		skip_frames = target;
		frame_position = target;
	}
	void set_parameter(const StringName &p_name, const Variant &p_value) override {
#ifdef MODULE_MEDIA_STREAM_ENABLED
		if (p_name == SNAME("_ffmpeg_prepare_reuse_seek")) {
			// Called only by the owning decode worker, before a finite seek.
			// Join the producer before touching its demuxer or resetting IO.
			packet_reader.stop(false);
			packet_reader.reset();
			read_ahead_used = false;
			return;
		}
		if (p_name == SNAME("_ffmpeg_start_read_ahead")) {
			if (!remote_cancel || !format || stream_index < 0) {
				failed = true;
				return;
			}
			if (!packet_reader.is_started()) {
				read_ahead_prefill_seconds = p_value.get_type() == Variant::FLOAT ? CLAMP(double(p_value), 0.5, 6.0) : 6.0;
				if (read_ahead_used || packet_reader.start(format, stream_index, remote_cancel) != OK) {
					failed = true;
					return;
				}
				read_ahead_used = true;
			}
			return;
		}
		if (p_name == SNAME("_ffmpeg_stop_read_ahead")) {
			packet_reader.stop();
			return;
		}
		if (p_name == SNAME("_ffmpeg_live_origin")) {
			if (!prepare_live_origin(double(p_value))) {
				failed = true;
			}
			return;
		}
#endif
		if (p_name == SNAME("looping")) {
			looping_override = p_value != Variant();
			looping = looping_override && bool(p_value);
		}
	}
	Variant get_parameter(const StringName &p_name) const override {
		if (p_name == SNAME("_ffmpeg_reusable_seek")) {
			return remote_cancel && format && format->pb && (format->pb->seekable & AVIO_SEEKABLE_NORMAL) && !remote_live_hls && !String(format->iformat->name).contains("hls");
		}
		if (p_name == SNAME("_ffmpeg_hls_timeline")) {
			Dictionary state;
			state["enabled"] = hls_pdt_timeline;
			state["trimmed_frames"] = timeline_trimmed_frames;
			state["padded_frames"] = timeline_padded_frames;
			return state;
		}
		if (p_name == SNAME("_ffmpeg_metadata")) {
			return stream_metadata.duplicate();
		}
		if (p_name == SNAME("stream_error")) {
			return failed ? int(ERR_INVALID_DATA) : int(OK);
		}
		if (p_name == SNAME("_ffmpeg_error_message")) {
			return decode_error;
		}
#ifdef MODULE_MEDIA_STREAM_ENABLED
		if (p_name == SNAME("_ffmpeg_can_read_ahead")) {
			return remote_cancel && format && stream_index >= 0 && FFmpegFileIO::is_remote(path);
		}
		if (p_name == SNAME("_ffmpeg_live_hls")) {
			return remote_live_hls;
		}
		if (p_name == SNAME("_ffmpeg_remote_origin")) {
			return input_origin_known ? Variant(remote_origin) : Variant();
		}
		if (p_name == SNAME("_ffmpeg_input_origin")) {
			return input_origin_known ? Variant(input_origin) : Variant();
		}
		if (p_name == SNAME("_ffmpeg_dvr_input_start")) {
			return file_io.get_dvr_input_start();
		}
		if (p_name == SNAME("_ffmpeg_read_ahead_ready")) {
			return packet_reader.is_started() && packet_reader.has_prefill(read_ahead_prefill_seconds);
		}
		if (p_name == SNAME("_ffmpeg_read_ahead_error")) {
			return packet_reader.is_started() ? packet_reader.get_terminal_error() : (failed ? AVERROR(EIO) : 0);
		}
		if (p_name == SNAME("_ffmpeg_packet_waiting")) {
			return packet_waiting;
		}
#endif
		return looping_override && p_name == SNAME("looping") ? Variant(looping) : Variant();
	}
	void tag_used_streams() override {
		if (loop_stream.is_valid()) {
			loop_stream->tag_used(get_playback_position());
		}
	}
#if defined(FFMPEG_TESTS_ENABLED) && defined(MODULE_MEDIA_STREAM_ENABLED)
	bool attach_test_packet_reader(FFmpegPacketReader::TestReadCallback p_read, void *p_userdata, const SafeFlag *p_seek_cancel, double p_seek_from) {
		// Exercise actual PCM decode/mix recovery with controlled packet supply.
		// No production path can attach a synthetic packet source.
		if (!format || !codec || stream_index < 0) {
			return false;
		}
		if (p_seek_cancel) {
			remote_cancel = p_seek_cancel;
			seek(p_seek_from);
			if (failed) {
				return false;
			}
		} else {
			avcodec_flush_buffers(codec);
			swr_free(&resampler);
			av_packet_unref(packet);
			av_frame_unref(frame);
			pcm_position = pcm_frames = 0;
			packet_pending = draining = eof = failed = rewind_pending = packet_waiting = false;
			frame_position = skip_frames = silence_frames = 0;
		}
		read_ahead_used = true;
		return packet_reader.start_for_test(p_read, p_userdata, stream_index, source_time_base) == OK;
	}
#endif
	int mix(AudioFrame *p_buffer, float, int p_frames) override {
		int mixed = 0;
		bool restarted_without_output = false;
		while (playing && mixed < p_frames) {
			if (silence_frames > 0) {
				int count = int(MIN(silence_frames, uint64_t(p_frames - mixed)));
				for (int i = 0; i < count; i++) {
					p_buffer[mixed++] = AudioFrame(0, 0);
				}
				silence_frames -= count;
				frame_position += count;
				continue;
			}
			const bool use_loop = looping_override ? looping : loop_stream.is_valid() && loop_stream->has_loop();
			uint64_t loop_end = UINT64_MAX;
			if (use_loop && loop_stream.is_valid() && loop_stream->get_bpm() > 0 && loop_stream->get_beat_count() > 0) {
				loop_end = uint64_t(loop_stream->get_beat_count() * double(output_sample_rate) * 60.0 / loop_stream->get_bpm());
			}
			const bool at_loop_end = frame_position >= loop_end;
			if (at_loop_end || !ensure_samples()) {
				if (!at_loop_end && packet_waiting) {
					break;
				}
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
			if (silence_frames > 0) {
				// ensure_samples() may discover a declared timeline gap.
				continue;
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

#ifdef MODULE_MEDIA_STREAM_ENABLED
#include "core/io/marshalls.h"
#include "core/os/thread.h"
#include "scene/resources/audio_stream_wav.h"

Error ffmpeg_decode_audio_pcm(const String &p_source, const Dictionary &p_headers, const SafeFlag &p_cancel, int p_mix_rate, int64_t p_max_pcm_bytes, uint64_t p_deadline, Dictionary &r_result, bool p_allow_http) {
	r_result.clear();
	if (p_mix_rate < 8000 || p_mix_rate > 192000 || p_max_pcm_bytes < 4 || p_max_pcm_bytes > 128 * 1024 * 1024 || !p_deadline) {
		return ERR_INVALID_PARAMETER;
	}
	auto interrupted_error = [&]() -> Error {
		return OS::get_singleton()->get_ticks_usec() >= p_deadline ? ERR_TIMEOUT : (p_cancel.is_set() ? ERR_SKIP : OK);
	};
	Error error = interrupted_error();
	if (error != OK) {
		return error;
	}
	Ref<AudioStreamPlaybackFFmpeg> decoder;
	decoder.instantiate();
	if (!decoder->open_pcm_source(p_source, p_headers, &p_cancel, p_mix_rate, p_deadline, p_allow_http)) {
		r_result["message"] = decoder->get_parameter(SNAME("_ffmpeg_error_message"));
		error = interrupted_error();
		return error != OK ? error : ERR_CANT_OPEN;
	}
	Vector<uint8_t> data;
	AudioFrame block[4096];
	int64_t frame_count = 0;
	while (true) {
		error = interrupted_error();
		if (error != OK) {
			return error;
		}
		const int requested = int(MIN(int64_t(4096), (p_max_pcm_bytes - frame_count * 4) / 4 + 1));
		const int count = decoder->read_pcm_samples(block, requested);
		error = interrupted_error();
		if (error != OK) {
			return error;
		}
		if (int(decoder->get_parameter(SNAME("stream_error"))) != OK) {
			r_result["message"] = decoder->get_parameter(SNAME("_ffmpeg_error_message"));
			return ERR_INVALID_DATA;
		}
		if (!count) {
			break;
		}
		const int64_t previous_bytes = frame_count * 4;
		if (int64_t(count) > (p_max_pcm_bytes - previous_bytes) / 4) {
			r_result["message"] = "Decoded audio exceeds the PCM byte limit; no partial stream was returned.";
			return ERR_OUT_OF_MEMORY;
		}
		const int64_t next_bytes = previous_bytes + int64_t(count) * 4;
		if (data.resize(next_bytes) != OK) {
			return ERR_OUT_OF_MEMORY;
		}
		uint8_t *write = data.ptrw() + previous_bytes;
		for (int i = 0; i < count; i++) {
			if (!Math::is_finite(block[i].left) || !Math::is_finite(block[i].right)) {
				return ERR_INVALID_DATA;
			}
			const int16_t left = int16_t(CLAMP(Math::round(block[i].left * 32767.0f), -32768.0f, 32767.0f));
			const int16_t right = int16_t(CLAMP(Math::round(block[i].right * 32767.0f), -32768.0f, 32767.0f));
			encode_uint16(uint16_t(left), write + i * 4);
			encode_uint16(uint16_t(right), write + i * 4 + 2);
		}
		frame_count += count;
	}
	if (!frame_count) {
		r_result["message"] = "No decoded audio samples were available.";
		return ERR_INVALID_DATA;
	}
	Ref<AudioStreamWAV> wav;
	wav.instantiate();
	wav->set_format(AudioStreamWAV::FORMAT_16_BITS);
	wav->set_mix_rate(p_mix_rate);
	wav->set_stereo(true);
	wav->set_data(data);
	error = interrupted_error();
	if (error != OK) {
		return error;
	}
	Dictionary metadata = decoder->get_parameter(SNAME("_ffmpeg_metadata"));
	metadata["duration"] = double(frame_count) / p_mix_rate;
	metadata["opened"] = true;
	r_result["stream"] = wav;
	r_result["duration"] = double(frame_count) / p_mix_rate;
	r_result["frame_count"] = frame_count;
	r_result["mix_rate"] = p_mix_rate;
	r_result["pcm_bytes"] = frame_count * 4;
	r_result["metadata"] = metadata;
	r_result["worker_thread"] = !Thread::is_main_thread();
	return OK;
}
#endif

void AudioStreamMedia::set_file(const String &p_file) {
	file = p_file;
	length = -1.0;
	remote_length_usec.set(0);
}

String AudioStreamMedia::get_file() const {
	return file;
}

Ref<AudioStreamPlayback> AudioStreamMedia::instantiate_playback() {
#ifdef MODULE_MEDIA_STREAM_ENABLED
	if (FFmpegFileIO::is_remote(file)) {
		return ffmpeg_async_audio(file, get_http_headers(), Ref<AudioStreamMedia>(this));
	}
#endif
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
Ref<AudioStreamPlayback> ffmpeg_audio_playback_from_buffer(const Vector<uint8_t> &p_data, const char *p_format, const String &p_path, const Ref<AudioStream> &p_loop_stream, const Dictionary &p_headers, const SafeFlag *p_cancel, double *r_length, FFmpegDVRWindow *p_dvr, bool p_dvr_primary, bool p_allow_http) {
	Ref<AudioStreamPlaybackFFmpeg> playback;
	playback.instantiate();
	playback->set_remote_options(p_headers, p_cancel, p_dvr, p_dvr_primary, p_allow_http);
	if (!playback->open_buffer(p_data, p_format, p_path)) {
		return Ref<AudioStreamPlayback>();
	}
#ifdef MODULE_MEDIA_STREAM_ENABLED
	if (p_dvr && p_dvr_primary && bool(p_dvr->snapshot().get("available", false))) {
		double input_start = playback->get_parameter(SNAME("_ffmpeg_dvr_input_start"));
		double offset = double(p_dvr->snapshot()["selected_utc"]) - input_start;
		Variant origin = playback->get_parameter(SNAME("_ffmpeg_remote_origin"));
		if (input_start < 0 || origin.get_type() == Variant::NIL) {
			return Ref<AudioStreamPlayback>();
		}
		playback->set_parameter(SNAME("_ffmpeg_live_origin"), double(origin) + offset);
		if (int(playback->get_parameter(SNAME("stream_error"))) != OK) {
			return Ref<AudioStreamPlayback>();
		}
	}
#endif
	playback->set_loop_stream(p_loop_stream);
	if (r_length) {
		*r_length = playback->get_decoded_length();
	}
	return playback;
}

#if defined(FFMPEG_TESTS_ENABLED) && defined(MODULE_MEDIA_STREAM_ENABLED)
Ref<AudioStreamPlayback> ffmpeg_test_audio_packet_reader(const Vector<uint8_t> &p_wave, FFmpegPacketReader::TestReadCallback p_read, void *p_userdata, const SafeFlag *p_seek_cancel, double p_seek_from) {
	Ref<AudioStreamPlaybackFFmpeg> playback;
	playback.instantiate();
	if (!playback->open_buffer(p_wave, nullptr, String()) || !playback->attach_test_packet_reader(p_read, p_userdata, p_seek_cancel, p_seek_from)) {
		return Ref<AudioStreamPlayback>();
	}
	return playback;
}
#endif

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
#ifdef MODULE_MEDIA_STREAM_ENABLED
	if (FFmpegFileIO::is_remote(file)) {
		return double(remote_length_usec.get()) / 1000000.0;
	}
#endif
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
	ClassDB::bind_method(D_METHOD("set_allow_http", "enabled"), &AudioStreamMedia::set_allow_http);
	ClassDB::bind_method(D_METHOD("is_http_allowed"), &AudioStreamMedia::is_http_allowed);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "allow_http"), "set_allow_http", "is_http_allowed");
	ClassDB::bind_method(D_METHOD("set_live_dvr_enabled", "enabled"), &AudioStreamMedia::set_live_dvr_enabled);
	ClassDB::bind_method(D_METHOD("is_live_dvr_enabled"), &AudioStreamMedia::is_live_dvr_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "live_dvr_enabled"), "set_live_dvr_enabled", "is_live_dvr_enabled");
#ifdef MODULE_MEDIA_STREAM_ENABLED
	ClassDB::bind_method(D_METHOD("set_http_headers", "headers"), &AudioStreamMedia::set_http_headers);
	ClassDB::bind_method(D_METHOD("get_http_headers"), &AudioStreamMedia::get_http_headers);
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "http_headers"), "set_http_headers", "get_http_headers");
#endif
	ClassDB::bind_method(D_METHOD("set_file", "file"), &AudioStreamMedia::set_file);
	ClassDB::bind_method(D_METHOD("get_file"), &AudioStreamMedia::get_file);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "file"), "set_file", "get_file");
}
