#include "video_stream_media.h"

#include "ffmpeg_file_io.h"
#include "ffmpeg_hardware_lifetime.h"
#include "ffmpeg_media_metadata.h"

#include "core/error/error_macros.h"
#include "core/io/file_access.h"
#include "core/io/resource_importer.h"
#include "core/object/class_db.h"
#include "core/os/memory.h"
#include "core/os/os.h"
#ifdef MODULE_MEDIA_STREAM_ENABLED
#include "ffmpeg_async_playback.h"
#include "ffmpeg_packet_reader.h"
#endif
#include "scene/resources/image_texture.h"
#include "servers/audio/audio_server.h"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

class VideoStreamPlaybackFFmpeg : public VideoStreamPlayback {
	GDCLASS(VideoStreamPlaybackFFmpeg, VideoStreamPlayback);

	FFmpegFileIO file_io;
	AVFormatContext *format = nullptr;
	AVCodecParameters *video_parameters = nullptr;
	AVRational video_time_base = { 0, 1 };
	int64_t input_start_time = AV_NOPTS_VALUE;
	bool live_hls = false;
#ifdef MODULE_MEDIA_STREAM_ENABLED
	FFmpegPacketReader packet_reader;
#endif
	AVCodecContext *codec = nullptr;
	AVPacket *packet = nullptr;
	AVFrame *frame = nullptr;
	AVFrame *queued_frame = nullptr;
	AVFrame *presentation_frame = nullptr;
	AVFrame *software_frame = nullptr;
	AVFrame *rgba_frame = nullptr;
	AVBufferRef *hardware_device = nullptr;
	SwsContext *scale = nullptr;
	int video_stream = -1;
	bool playing = false;
	bool paused = false;
	double position = 0.0;
	double length = 0.0;
	Dictionary stream_metadata;
	double playback_clock = 0.0;
	double audio_frames_due = 0.0;
	double audio_position = 0.0;
	bool audio_clock_available = false;
	AudioPlaybackPositionCallback audio_clock_callback = nullptr;
	void *audio_clock_userdata = nullptr;
	bool seek_preroll = false;
	double seek_target = 0.0;
	bool live_start_prepared = false;
	bool preparing_live_start = false;
	Vector<float> pending_audio;
	int pending_audio_offset = 0;
	bool packet_pending = false;
	bool packet_waiting = false;
	int64_t last_packet_dts = AV_NOPTS_VALUE;
	bool demux_eof = false;
	bool decoder_draining = false;
	bool hardware_decoding = false;
	uint64_t presented_frames = 0;
	bool async_frames = false;
	bool frame_queue_full = false;
	double decode_ahead_seconds = 0.05;
	Ref<Image> decoded_image;
	bool waiting_for_keyframe = false;
#ifdef DEV_ENABLED
	struct DebugStage {
		uint64_t usec = 0;
		uint64_t maximum_usec = 0;
		uint64_t calls = 0;
		Dictionary snapshot() const {
			Dictionary value;
			value["usec"] = int64_t(usec);
			value["maximum_usec"] = int64_t(maximum_usec);
			value["calls"] = int64_t(calls);
			return value;
		}
	};
	struct DebugMeasure {
		DebugStage &stage;
		uint64_t started = OS::get_singleton()->get_ticks_usec();
		bool running = true;
		explicit DebugMeasure(DebugStage &p_stage) : stage(p_stage) {}
		void stop() {
			if (running) {
				const uint64_t elapsed = OS::get_singleton()->get_ticks_usec() - started;
				stage.usec += elapsed;
				stage.maximum_usec = MAX(stage.maximum_usec, elapsed);
				stage.calls++;
				running = false;
			}
		}
		~DebugMeasure() { stop(); }
	};
	DebugStage debug_update_time, debug_audio_time, debug_receive_time, debug_send_time, debug_transfer_time, debug_convert_time, debug_texture_time;
	bool test_hardware_failure = false;
	int test_delay_above_height = 0;
	uint64_t test_video_delay_usec = 0;
	double debug_last_frame_position = 0.0;
	int64_t debug_negative_frames = 0;
	int64_t debug_decoded_frames = 0;
	int64_t debug_late_discards = 0;
	double debug_first_presented_position = -1.0;
#endif
	Ref<ImageTexture> texture;
	Ref<AudioStreamPlayback> audio_playback;
	Dictionary network_headers;
	const SafeFlag *network_cancel = nullptr;
	double timestamp_origin = 0.0;
	Error stream_error = OK;
	String stream_error_message;

	static bool _valid_remote_video_size(int p_width, int p_height) {
		return p_width > 0 && p_height > 0 && int64_t(p_width) * p_height <= 16777216;
	}

	bool _open_failed(String *r_error, const String &p_message) {
		if (r_error) {
			*r_error = p_message;
		}
		close();
		return false;
	}

	double _get_target_clock() const {
		if (network_cancel && audio_playback.is_valid() && audio_playback->is_playing() && !seek_preroll) {
			const double current = audio_clock_callback ? audio_clock_callback(audio_clock_userdata) : (audio_clock_available ? audio_position : -1.0);
			if (Math::is_finite(current) && current >= 0.0) {
				return current;
			}
		}
		return playback_clock - (audio_playback.is_valid() ? audio_frames_due / AudioServer::get_singleton()->get_mix_rate() : 0.0);
	}

	static bool _is_hardware_format(AVPixelFormat p_format) {
		const AVPixFmtDescriptor *descriptor = av_pix_fmt_desc_get(p_format);
		return descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL);
	}

	static AVPixelFormat _get_software_format(AVCodecContext *, const AVPixelFormat *p_formats) {
		for (const AVPixelFormat *format = p_formats; *format != AV_PIX_FMT_NONE; format++) {
			if (!_is_hardware_format(*format)) {
				return *format;
			}
		}
		return AV_PIX_FMT_NONE;
	}

	bool _open_decoder(const AVCodec *p_decoder, AVBufferRef *p_device, bool p_hardware) {
		if (!p_decoder) {
			return false;
		}
		AVCodecContext *candidate = avcodec_alloc_context3(p_decoder);
		if (!candidate) {
			return false;
		}
		if (avcodec_parameters_to_context(candidate, video_parameters) < 0) {
			avcodec_free_context(&candidate);
			return false;
		}
		candidate->pkt_timebase = video_time_base;
		if (network_cancel) {
			// Enforce the remote pixel limit in the decoder before it allocates
			// frames, including dimensions learned from later stream headers.
			candidate->max_pixels = 16777216;
		}
		candidate->get_format = p_device ? _get_format : (p_hardware ? avcodec_default_get_format : _get_software_format);
#if defined(WEB_ENABLED) && !defined(THREADS_ENABLED)
		candidate->thread_count = 1;
#endif
		if (p_device) {
			// Keep room for the caller's current, queued and presentation
			// references in addition to the decoder's own reference pictures.
			candidate->extra_hw_frames = 3;
			candidate->hw_device_ctx = av_buffer_ref(p_device);
			if (!candidate->hw_device_ctx) {
				avcodec_free_context(&candidate);
				return false;
			}
		}
		if (avcodec_open2(candidate, p_decoder, nullptr) < 0) {
			avcodec_free_context(&candidate);
			return false;
		}
		codec = candidate;
		hardware_decoding = p_hardware;
		print_verbose(vformat("FFmpeg: opened %s decoder '%s'.", p_hardware ? "hardware" : "software", p_decoder->name));
		return true;
	}

	bool _open_software_decoder() {
		const AVCodecID id = video_parameters->codec_id;
		// FFmpeg's native AV1 decoder has no CPU reconstruction backend.
		const AVCodec *decoder = id == AV_CODEC_ID_AV1 ? avcodec_find_decoder_by_name("libdav1d") : avcodec_find_decoder(id);
		if (decoder && !(decoder->capabilities & (AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_HYBRID))) {
			return _open_decoder(decoder, nullptr, false);
		}
		return false;
	}

	bool _try_open_hardware_decoder() {
#if defined(WINDOWS_ENABLED) || defined(ANDROID_ENABLED) || defined(LINUXBSD_ENABLED) || defined(MACOS_ENABLED) || defined(APPLE_EMBEDDED_ENABLED)
		const AVHWDeviceType device_types[] = {
#if defined(WINDOWS_ENABLED)
			// Prefer the established backends with CPU frame readback. D3D12's
			// reference-only path has failed on sustained H.264 live streams;
			// retain it as a final option rather than selecting it first.
			AV_HWDEVICE_TYPE_D3D11VA,
			AV_HWDEVICE_TYPE_DXVA2,
			AV_HWDEVICE_TYPE_CUDA,
			AV_HWDEVICE_TYPE_QSV,
			AV_HWDEVICE_TYPE_AMF,
			AV_HWDEVICE_TYPE_D3D12VA,
#elif defined(ANDROID_ENABLED)
			AV_HWDEVICE_TYPE_MEDIACODEC,
#elif defined(LINUXBSD_ENABLED)
			AV_HWDEVICE_TYPE_CUDA,
			AV_HWDEVICE_TYPE_QSV,
			AV_HWDEVICE_TYPE_VAAPI,
			AV_HWDEVICE_TYPE_VULKAN,
			AV_HWDEVICE_TYPE_AMF,
#elif defined(MACOS_ENABLED) || defined(APPLE_EMBEDDED_ENABLED)
			AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#endif
		};

		const AVCodecID id = video_parameters->codec_id;
		for (AVHWDeviceType device_type : device_types) {
			void *iterator = nullptr;
			const AVCodec *decoder = nullptr;
			while ((decoder = av_codec_iterate(&iterator))) {
				if (!av_codec_is_decoder(decoder) || decoder->id != id) {
					continue;
				}
				for (int index = 0; const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, index); index++) {
					if (config->device_type != device_type || !(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
						continue;
					}
					// MediaCodec without a Surface returns CPU-readable buffers.
					// Its opaque Surface frames cannot be transferred with FFmpeg.
					if (device_type == AV_HWDEVICE_TYPE_MEDIACODEC) {
						if (_open_decoder(decoder, nullptr, true)) {
							return true;
						}
						break;
					}
					AVBufferRef *device = nullptr;
					const int device_error = av_hwdevice_ctx_create(&device, device_type, nullptr, nullptr, 0);
					ffmpeg_retain_wsl_d3d12_driver();
					if (device_error >= 0 && _open_decoder(decoder, device, true)) {
						hardware_device = device;
						return true;
					}
					av_buffer_unref(&device);
					break;
				}
			}
		}
#endif
		return false;
	}

	static AVPixelFormat _get_format(AVCodecContext *p_codec, const AVPixelFormat *p_formats) {
		AVHWDeviceType device_type = AV_HWDEVICE_TYPE_NONE;
		if (p_codec->hw_device_ctx) {
			AVHWDeviceContext *device = reinterpret_cast<AVHWDeviceContext *>(p_codec->hw_device_ctx->data);
			device_type = device->type;
		}
		for (int index = 0; const AVCodecHWConfig *config = avcodec_get_hw_config(p_codec->codec, index); index++) {
			if (config->device_type != device_type) {
				continue;
			}
			for (const AVPixelFormat *format = p_formats; *format != AV_PIX_FMT_NONE; format++) {
				if (*format == config->pix_fmt) {
					return *format;
				}
			}
		}
		return AV_PIX_FMT_NONE;
	}

	bool _fallback_to_software(const char *p_reason) {
		if (!hardware_decoding) {
			return false;
		}
		hardware_decoding = false;
		print_verbose(vformat("FFmpeg: %s; falling back to software at %.3f seconds.", p_reason, position));
		av_frame_unref(frame);
		av_frame_unref(queued_frame);
		av_frame_unref(presentation_frame);
		av_frame_unref(software_frame);
		av_packet_unref(packet);
		avcodec_free_context(&codec);
		av_buffer_unref(&hardware_device);
		if (!_open_software_decoder()) {
			return false;
		}
		const AVRational time_base = video_time_base;
		if (live_hls
#ifdef MODULE_MEDIA_STREAM_ENABLED
				|| packet_reader.is_started()
#endif
		) {
			// A live playlist may no longer contain our last presented frame,
			// and a packet producer owns all format reads until it is stopped.
			// Continue the existing demuxer at its next independent frame;
			// never reset the paired audio cursor or shared source-PTS origin.
			waiting_for_keyframe = true;
		} else if (av_seek_frame(format, video_stream, int64_t((position + timestamp_origin) / av_q2d(time_base)), AVSEEK_FLAG_BACKWARD) < 0) {
			if (!network_cancel) {
				return false;
			}
			waiting_for_keyframe = true;
		} else {
			waiting_for_keyframe = false;
			last_packet_dts = AV_NOPTS_VALUE;
		}
		// Re-decode the preceding keyframe without resetting the audio cursor,
		// accepted samples, or playback clock. Catch-up discards stale frames.
		packet_pending = false;
		demux_eof = false;
		decoder_draining = false;
		return true;
	}

	void _mix_audio(double p_delta) {
		if (!audio_playback.is_valid() || !mix_callback) {
			return;
		}
		audio_frames_due += MAX(p_delta, 0.0) * AudioServer::get_singleton()->get_mix_rate();
		if (live_hls) {
			// Rejected PCM remains queued. Retain at most one second of
			// production credit instead of accumulating stalled wall time.
			audio_frames_due = MIN(audio_frames_due, double(AudioServer::get_singleton()->get_mix_rate()));
		}
		// Preserve rejected samples and fractional durations; bound work after stalls.
		for (int batch = 0; batch < 16; batch++) {
			if (pending_audio_offset < pending_audio.size() / 2) {
				const int remaining = pending_audio.size() / 2 - pending_audio_offset;
				const int accepted = mix_callback(mix_udata, pending_audio.ptr() + pending_audio_offset * 2, remaining);
				pending_audio_offset += accepted;
				audio_frames_due -= accepted;
				if (accepted < remaining) {
					return;
				}
			}
			pending_audio.clear();
			pending_audio_offset = 0;
			const int mix_frames = int(MIN(audio_frames_due, 1024.0));
			if (mix_frames <= 0) {
				return;
			}
			Vector<AudioFrame> samples;
			samples.resize(mix_frames);
			const int mixed = audio_playback->mix(samples.ptrw(), 1.0, mix_frames);
			if (mixed <= 0) {
				if (network_cancel && int(audio_playback->get_parameter(SNAME("stream_error"))) != OK) {
					stream_error = ERR_CONNECTION_ERROR;
					stream_error_message = audio_playback->get_parameter(SNAME("_ffmpeg_error_message"));
					playing = false;
				}
				// Retry only the outstanding source samples after a temporary
				// packet underrun, without accumulating the stalled wall time.
				if (!network_cancel || !bool(audio_playback->get_parameter(SNAME("_ffmpeg_packet_waiting")))) {
					audio_frames_due = 0.0;
				}
				return;
			}
			pending_audio.resize(mixed * 2);
			float *pcm = pending_audio.ptrw();
			for (int i = 0; i < mixed; i++) {
				pcm[i * 2] = samples[i].left;
				pcm[i * 2 + 1] = samples[i].right;
			}
		}
	}
	void close() {
#ifdef MODULE_MEDIA_STREAM_ENABLED
		packet_reader.stop();
#endif
		if (scale) {
			sws_freeContext(scale);
			scale = nullptr;
		}
		if (codec) {
			avcodec_free_context(&codec);
		}
		if (format) {
			avformat_close_input(&format);
		}
		avcodec_parameters_free(&video_parameters);
		file_io.close();
		if (packet) {
			av_packet_free(&packet);
		}
		if (frame) {
			av_frame_free(&frame);
		}
		if (queued_frame) {
			av_frame_free(&queued_frame);
		}
		if (presentation_frame) {
			av_frame_free(&presentation_frame);
		}
		if (software_frame) {
			av_frame_free(&software_frame);
		}
		if (rgba_frame) {
			av_frame_free(&rgba_frame);
		}
		if (hardware_device) {
			av_buffer_unref(&hardware_device);
		}
		video_stream = -1;
		video_time_base = { 0, 1 };
		input_start_time = AV_NOPTS_VALUE;
		live_hls = false;
		playing = false;
		playback_clock = 0.0;
		audio_frames_due = 0.0;
		pending_audio.clear();
		pending_audio_offset = 0;
		packet_pending = false;
		packet_waiting = false;
		last_packet_dts = AV_NOPTS_VALUE;
		demux_eof = false;
		decoder_draining = false;
		hardware_decoding = false;
		waiting_for_keyframe = false;
		live_start_prepared = false;
		preparing_live_start = false;
		audio_playback.unref();
	}

public:
	~VideoStreamPlaybackFFmpeg() override { close(); }
	Dictionary get_stream_metadata() const override { return stream_metadata.duplicate(); }
	void use_async_frames() { async_frames = true; }
	bool _set(const StringName &p_name, const Variant &p_value) {
		if (p_name == SNAME("_ffmpeg_frame_queue_full")) {
			frame_queue_full = p_value;
			return true;
		}
		return false;
	}

	bool _get(const StringName &p_name, Variant &r_result) const {
		if (p_name == SNAME("_ffmpeg_async_frames")) {
			r_result = async_frames;
			return true;
		}
		if (p_name == SNAME("_ffmpeg_cpu_frame")) {
			r_result = decoded_image;
			return true;
		}
		if (p_name == SNAME("_ffmpeg_audio_active")) {
			r_result = audio_playback.is_valid() && audio_playback->is_playing();
			return true;
		}
		if (p_name == SNAME("_ffmpeg_metadata")) {
			r_result = stream_metadata.duplicate();
			return true;
		}
#ifdef DEV_ENABLED
		if (p_name == SNAME("_ffmpeg_debug_state")) {
			Dictionary state;
			state["position"] = position;
			state["target_clock"] = _get_target_clock();
			state["playback_clock"] = playback_clock;
			state["origin"] = timestamp_origin;
			state["last_decoded_position"] = debug_last_frame_position;
			state["decoded_frames"] = debug_decoded_frames;
			state["uploaded_frames"] = async_frames ? int64_t(0) : int64_t(presented_frames);
			state["converted_frames"] = int64_t(presented_frames);
			state["cpu_frame_queue"] = async_frames;
			state["late_discards"] = debug_late_discards;
			state["first_uploaded_position"] = debug_first_presented_position;
			state["hardware_decoding"] = hardware_decoding;
			state["codec"] = codec && codec->codec ? String(codec->codec->name) : String();
			Dictionary stages;
			stages["update"] = debug_update_time.snapshot();
			stages["audio"] = debug_audio_time.snapshot();
			stages["receive"] = debug_receive_time.snapshot();
			stages["send"] = debug_send_time.snapshot();
			stages["transfer"] = debug_transfer_time.snapshot();
			stages["convert"] = debug_convert_time.snapshot();
			stages["texture"] = debug_texture_time.snapshot();
			state["stages"] = stages;
			state["negative_frames"] = debug_negative_frames;
			state["playing"] = playing;
			state["audio_playing"] = audio_playback.is_valid() && audio_playback->is_playing();
			state["audio_due"] = audio_frames_due;
			state["waiting_for_keyframe"] = waiting_for_keyframe;
			state["packet_waiting"] = packet_waiting;
			state["packet_pending"] = packet_pending;
			state["demux_eof"] = demux_eof;
			state["decoder_draining"] = decoder_draining;
			state["queued_format"] = queued_frame ? queued_frame->format : -1;
			state["queued_pts"] = queued_frame ? queued_frame->best_effort_timestamp : AV_NOPTS_VALUE;
#ifdef MODULE_MEDIA_STREAM_ENABLED
			state["reader_terminal"] = packet_reader.get_terminal_error();
			state["reader_prefill"] = packet_reader.has_prefill();
#endif
			r_result = state;
			return true;
		}
#endif
		if (p_name == SNAME("stream_error")) {
			r_result = int(stream_error);
			return true;
		}
		if (p_name == SNAME("_ffmpeg_error_message")) {
			r_result = stream_error_message;
			return true;
		}
		if (p_name == SNAME("_ffmpeg_video_lateness")) {
			r_result = MAX(0.0, _get_target_clock() - position);
			return true;
		}
		if (p_name == SNAME("_ffmpeg_presented_frames")) {
			r_result = int64_t(presented_frames);
			return true;
		}
		if (p_name == SNAME("_ffmpeg_audio_backlog")) {
			r_result = MAX(0.0, audio_frames_due / AudioServer::get_singleton()->get_mix_rate());
			return true;
		}
#ifdef MODULE_MEDIA_STREAM_ENABLED
		if (p_name == SNAME("_ffmpeg_live_hls")) {
			r_result = live_hls;
			return true;
		}
		if (p_name == SNAME("_ffmpeg_packet_waiting")) {
			r_result = packet_waiting || (audio_playback.is_valid() && bool(audio_playback->get_parameter(SNAME("_ffmpeg_packet_waiting"))));
			return true;
		}
		if (p_name == SNAME("_ffmpeg_presentable_position")) {
			double presentable = position;
			if (queued_frame && queued_frame->format >= 0 && queued_frame->best_effort_timestamp != AV_NOPTS_VALUE && format && video_stream >= 0) {
				const double queued_position = queued_frame->best_effort_timestamp * av_q2d(video_time_base) - timestamp_origin;
				if (Math::is_finite(queued_position)) {
					presentable = MAX(presentable, queued_position);
				}
			}
			r_result = presentable;
			return true;
		}
#endif
#if defined(DEV_ENABLED) && defined(MODULE_MEDIA_STREAM_ENABLED)
		if (p_name == SNAME("_ffmpeg_live_origin")) {
			Dictionary origins;
			origins["live_hls"] = live_hls;
			origins["video_source_origin"] = input_start_time != AV_NOPTS_VALUE ? Variant(double(input_start_time) / AV_TIME_BASE) : Variant();
			origins["video_clock_origin"] = timestamp_origin;
			origins["audio_source_origin"] = audio_playback.is_valid() ? audio_playback->get_parameter(SNAME("_ffmpeg_input_origin")) : Variant();
			origins["audio_clock_origin"] = audio_playback.is_valid() ? audio_playback->get_parameter(SNAME("_ffmpeg_remote_origin")) : Variant();
			origins["audio_live_hls"] = audio_playback.is_valid() && bool(audio_playback->get_parameter(SNAME("_ffmpeg_live_hls")));
			r_result = origins;
			return true;
		}
#endif
		return false;
	}

	bool open_file(const String &p_path, const Dictionary &p_headers = Dictionary(), const SafeFlag *p_cancel = nullptr, const String &p_audio_file = String(), const Dictionary &p_audio_headers = Dictionary(), String *r_error = nullptr, FFmpegDVRWindow *p_dvr = nullptr) {
		close();
		if (r_error) {
			*r_error = String();
		}
		stream_error = OK;
		stream_error_message = String();
		network_headers = p_headers;
		network_cancel = p_cancel;
#ifdef MODULE_MEDIA_STREAM_ENABLED
		packet_reader.reset();
		file_io.set_remote_options(p_headers, p_cancel, packet_reader.get_stop_flag());
		file_io.set_dvr(Ref<FFmpegDVRWindow>(p_dvr), true);
#else
		file_io.set_remote_options(p_headers, p_cancel);
#endif
		const int format_error = file_io.open(&format, p_path);
		if (format_error < 0) {
			return _open_failed(r_error, vformat("Could not open the video media container (FFmpeg error %d).", format_error));
		}
		const int info_error = avformat_find_stream_info(format, nullptr);
		if (info_error < 0) {
			return _open_failed(r_error, vformat("Could not inspect the video streams (FFmpeg error %d).", info_error));
		}
		video_stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
		if (video_stream < 0) {
			return _open_failed(r_error, vformat("No decodable video stream was found (FFmpeg error %d).", video_stream));
		}
#ifdef MODULE_MEDIA_STREAM_ENABLED
		if (network_cancel && !_valid_remote_video_size(format->streams[video_stream]->codecpar->width, format->streams[video_stream]->codecpar->height)) {
			// A bounded master-playlist probe can leave some higher-ranked
			// renditions without dimensions. Prefer a known, bounded video
			// alternative instead of rejecting the entire playable input.
			int fallback_stream = -1;
			int64_t fallback_pixels = 0;
			for (unsigned int i = 0; i < format->nb_streams; i++) {
				const AVStream *stream = format->streams[i];
				const AVCodecParameters *parameters = stream->codecpar;
				if (parameters->codec_type != AVMEDIA_TYPE_VIDEO || (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) || !_valid_remote_video_size(parameters->width, parameters->height) || !avcodec_find_decoder(parameters->codec_id)) {
					continue;
				}
				const int64_t pixels = int64_t(parameters->width) * parameters->height;
				if (pixels > fallback_pixels) {
					fallback_stream = int(i);
					fallback_pixels = pixels;
				}
			}
			if (fallback_stream >= 0) {
				video_stream = fallback_stream;
			}
		}
		if (network_cancel && format->iformat && String(format->iformat->name) == "hls") {
			// HLS uses stream discard flags to stop fetching unused variants.
			// Reading every rendition can starve the one we actually decode.
			for (unsigned int i = 0; i < format->nb_streams; i++) {
				format->streams[i]->discard = int(i) == video_stream ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
			}
		}
#endif
#ifdef MODULE_MEDIA_STREAM_ENABLED
		timestamp_origin = p_cancel && format->start_time != AV_NOPTS_VALUE ? double(format->start_time) / AV_TIME_BASE : 0.0;
#endif
		video_time_base = format->streams[video_stream]->time_base;
		input_start_time = format->start_time;
		live_hls = network_cancel && format->iformat && String(format->iformat->name) == "hls" && format->duration <= 0;
		video_parameters = avcodec_parameters_alloc();
		if (!video_parameters || avcodec_parameters_copy(video_parameters, format->streams[video_stream]->codecpar) < 0) {
			return _open_failed(r_error, "Could not copy the selected video stream parameters.");
		}
		const bool force_software = OS::get_singleton()->get_environment("GODOT_FFMPEG_FORCE_SOFTWARE") == "1";
#ifdef DEV_ENABLED
		test_hardware_failure = OS::get_singleton()->get_environment("GODOT_FFMPEG_TEST_HW_FAILURE") == "1";
		test_delay_above_height = OS::get_singleton()->get_environment("GODOT_FFMPEG_TEST_SLOW_VIDEO_ABOVE_HEIGHT").to_int();
		test_video_delay_usec = uint64_t(CLAMP(OS::get_singleton()->get_environment("GODOT_FFMPEG_TEST_VIDEO_DELAY_MSEC").to_int(), 0, 200)) * 1000;
#endif
		if ((force_software || !_try_open_hardware_decoder()) && !_open_software_decoder()) {
			return _open_failed(r_error, vformat("Could not open the video decoder for stream %d (codec %d).", video_stream, int(video_parameters->codec_id)));
		}
#ifdef MODULE_MEDIA_STREAM_ENABLED
		if (FFmpegFileIO::is_remote(p_path) && !_valid_remote_video_size(codec->width, codec->height)) {
			return _open_failed(r_error, vformat("Unsupported video dimensions %dx%d for stream %d (maximum %d pixels).", codec->width, codec->height, video_stream, 16777216));
		}
#endif
		packet = av_packet_alloc();
		frame = av_frame_alloc();
		queued_frame = av_frame_alloc();
		presentation_frame = av_frame_alloc();
		software_frame = av_frame_alloc();
		rgba_frame = av_frame_alloc();
		if (!packet || !frame || !queued_frame || !presentation_frame || !software_frame || !rgba_frame) {
			return _open_failed(r_error, vformat("Could not allocate the video playback frames (error %d).", int(ERR_OUT_OF_MEMORY)));
		}
		if (packet && frame && queued_frame) {
			Ref<Image> image = Image::create_empty(codec->width, codec->height, false, Image::FORMAT_RGBA8);
			texture = ImageTexture::create_from_image(image);
		}
#ifdef MODULE_MEDIA_STREAM_ENABLED
#ifdef DEV_ENABLED
		// Let the live-origin regression force the second input onto a later
		// playlist edge without delaying cancellation of the opening worker.
		if (live_hls) {
			const int delay_ms = CLAMP(OS::get_singleton()->get_environment("GODOT_FFMPEG_TEST_LIVE_AUDIO_DELAY_MSEC").to_int(), 0, 10000);
			const uint64_t delay_until = OS::get_singleton()->get_ticks_usec() + uint64_t(delay_ms) * 1000;
			while (OS::get_singleton()->get_ticks_usec() < delay_until) {
				if (network_cancel->is_set()) {
					return _open_failed(r_error, "Video opening was cancelled.");
				}
				OS::get_singleton()->delay_usec(10000);
			}
		}
#endif
#endif
		audio_playback = ffmpeg_audio_playback_from_buffer(Vector<uint8_t>(), nullptr, p_audio_file.is_empty() ? p_path : p_audio_file, Ref<AudioStream>(), p_audio_file.is_empty() ? network_headers : p_audio_headers, network_cancel, nullptr, p_dvr, false);
		if (!p_audio_file.is_empty() && audio_playback.is_null()) {
			return _open_failed(r_error, "Could not open the paired audio input.");
		}
#ifdef MODULE_MEDIA_STREAM_ENABLED
		if (p_dvr && bool(p_dvr->snapshot().get("available", false)) && (audio_playback.is_null() || !live_hls || !bool(audio_playback->get_parameter(SNAME("_ffmpeg_live_hls"))))) {
			return _open_failed(r_error, "Could not prepare the paired live inputs for YouTube DVR.");
		}
		if (live_hls && format->start_time != AV_NOPTS_VALUE && audio_playback.is_valid() && bool(audio_playback->get_parameter(SNAME("_ffmpeg_live_hls")))) {
			const Variant audio_origin = audio_playback->get_parameter(SNAME("_ffmpeg_remote_origin"));
			if (audio_origin.get_type() != Variant::NIL && Math::is_finite(double(audio_origin))) {
				const double original_origin = timestamp_origin;
				// Either input can start earlier. Prepare both at the later
				// source PTS before publishing readiness, rather than playing
				// old audio or waiting silently for an initial video frame.
				timestamp_origin = MAX(original_origin, double(audio_origin));
				if (p_dvr && bool(p_dvr->snapshot().get("available", false))) {
					const double selected = p_dvr->snapshot()["selected_utc"];
					const double video_start = file_io.get_dvr_input_start();
					const double audio_start = audio_playback->get_parameter(SNAME("_ffmpeg_dvr_input_start"));
					if (video_start < 0 || audio_start < 0) {
						return _open_failed(r_error, "The YouTube DVR inputs have no common advertised timeline.");
					}
					const double video_target = original_origin + selected - video_start;
					const double audio_target = double(audio_origin) + selected - audio_start;
					if (Math::abs(video_target - audio_target) > 0.15) {
						return _open_failed(r_error, "The YouTube DVR audio/video timestamps do not agree with their advertised timeline.");
					}
					timestamp_origin = MAX(video_target, audio_target);
				}
				if (timestamp_origin - original_origin > 30.0) {
					return _open_failed(r_error, vformat("Live video/audio source starts differ by %.6f seconds (maximum 30.0 seconds).", timestamp_origin - original_origin));
				}
				audio_playback->set_parameter(SNAME("_ffmpeg_live_origin"), timestamp_origin);
				if (int(audio_playback->get_parameter(SNAME("stream_error"))) != OK) {
					return _open_failed(r_error, vformat("Could not align live audio to the shared source clock (error %d; source gap %.6f seconds).", int(audio_playback->get_parameter(SNAME("stream_error"))), Math::abs(original_origin - double(audio_origin))));
				}
				seek_target = 0.0;
				seek_preroll = true;
				position = 0.0;
				playback_clock = 0.0;
				playing = true;
				paused = false;
				preparing_live_start = true;
				const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 30000000;
				while (seek_preroll && playing && stream_error == OK && !network_cancel->is_set() && OS::get_singleton()->get_ticks_usec() < deadline) {
					update(0.0);
				}
				const bool prepared = !seek_preroll && playing && stream_error == OK && !network_cancel->is_set();
				const bool stopped_during_preroll = !playing;
				preparing_live_start = false;
				playing = false;
				if (!prepared) {
					if (network_cancel->is_set()) {
						return _open_failed(r_error, "Live video opening was cancelled.");
					}
					if (stream_error != OK) {
						return _open_failed(r_error, stream_error_message.is_empty() ? vformat("Could not prepare the first live video frame (error %d).", int(stream_error)) : stream_error_message);
					}
					if (stopped_during_preroll) {
						return _open_failed(r_error, "The live video input ended before reaching the shared source clock.");
					}
					return _open_failed(r_error, "Timed out preparing live video at the shared source clock (30 seconds).");
				}
				live_start_prepared = true;
				print_verbose(vformat("FFmpeg: live HLS origins video %.6f, audio %.6f, shared %.6f.", original_origin, double(audio_origin), timestamp_origin));
			}
		}
#endif
		length = format->duration > 0 ? double(format->duration) / AV_TIME_BASE : 0.0;
		stream_metadata = FFmpegMediaMetadata::container(format, video_stream, p_path, length, live_hls);
		if (!packet || !frame || !queued_frame || !presentation_frame || !software_frame || !rgba_frame) {
			return _open_failed(r_error, "The video playback frames became unavailable during opening.");
		}
		return true;
	}

	bool start_read_ahead(String *r_error) {
#ifdef MODULE_MEDIA_STREAM_ENABLED
		if (!network_cancel || packet_reader.is_started()) {
			return true;
		}
		const bool audio_read_ahead = audio_playback.is_valid() && bool(audio_playback->get_parameter(SNAME("_ffmpeg_can_read_ahead")));
		const Error started = packet_reader.start(format, video_stream, network_cancel);
		if (started != OK) {
			return _open_failed(r_error, vformat("Could not start remote video packet read-ahead (error %d).", int(started)));
		}
		// Both producers run while the decoder worker waits for initial data.
		// No decoder, renderer or mixer call owns a network refill afterwards.
		if (audio_read_ahead) {
			audio_playback->set_parameter(SNAME("_ffmpeg_start_read_ahead"), true);
		}
		const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 30000000;
		while (!network_cancel->is_set()) {
			const bool video_ready = packet_reader.has_prefill(6.0);
			const bool audio_ready = !audio_read_ahead || bool(audio_playback->get_parameter(SNAME("_ffmpeg_read_ahead_ready")));
			if (video_ready && audio_ready) {
				return true;
			}
			if (!video_ready && packet_reader.get_terminal_error() < 0) {
				return _open_failed(r_error, vformat("Remote video packet read-ahead ended before buffering (FFmpeg error %d).", packet_reader.get_terminal_error()));
			}
			if (audio_read_ahead && (int(audio_playback->get_parameter(SNAME("stream_error"))) != OK || (!audio_ready && int(audio_playback->get_parameter(SNAME("_ffmpeg_read_ahead_error"))) < 0))) {
				return _open_failed(r_error, "Remote audio packet read-ahead failed before buffering.");
			}
			if (OS::get_singleton()->get_ticks_usec() >= deadline) {
				return _open_failed(r_error, "Timed out buffering remote video/audio packets.");
			}
			OS::get_singleton()->delay_usec(2000);
		}
		return _open_failed(r_error, "Live video/audio packet buffering was cancelled.");
#else
		return true;
#endif
	}

	void update(double p_delta) override {
		if (!playing || paused || !format || !codec) {
			return;
		}
#ifdef DEV_ENABLED
		DebugMeasure update_measure(debug_update_time);
		if (test_delay_above_height > 0 && codec->height > test_delay_above_height && test_video_delay_usec > 0) {
			OS::get_singleton()->delay_usec(test_video_delay_usec);
		}
#endif
		playback_clock += MAX(p_delta, 0.0);
		{
#ifdef DEV_ENABLED
			DebugMeasure audio_measure(debug_audio_time);
#endif
			_mix_audio(p_delta);
		}
		if (!playing) {
			return;
		}
		if (async_frames && frame_queue_full && !seek_preroll && !preparing_live_start) {
			return;
		}
		const double lead = async_frames && !seek_preroll && !preparing_live_start ? decode_ahead_seconds : 0.0;
		double target_clock = _get_target_clock() + lead;
		av_frame_unref(presentation_frame);
		const uint64_t decode_start = OS::get_singleton()->get_ticks_usec();
		bool frame_ready = false;
		double frame_position = position;
		if (queued_frame->format >= 0) {
			const double queued_position = queued_frame->best_effort_timestamp == AV_NOPTS_VALUE ? position : queued_frame->best_effort_timestamp * av_q2d(video_time_base) - timestamp_origin;
			if (queued_position > target_clock) {
				return;
			}
			av_frame_unref(frame);
			av_frame_move_ref(frame, queued_frame);
			frame_position = queued_position;
			frame_ready = true;
			if (network_cancel) {
				av_frame_ref(presentation_frame, frame);
			}
		}
		while (!frame_ready || (!async_frames && frame_position < target_clock - (network_cancel ? 0.0 : 0.1))) {
			// Decode reference frames, but avoid uploading stale frames. Limit catch-up work.
			if (OS::get_singleton()->get_ticks_usec() - decode_start >= 8000) {
				break;
			}
			if (frame_ready) {
				av_frame_unref(frame);
				frame_ready = false;
			}
			int receive_error;
			{
#ifdef DEV_ENABLED
				DebugMeasure receive_measure(debug_receive_time);
#endif
				receive_error = avcodec_receive_frame(codec, frame);
			}
			if (receive_error == AVERROR_EOF) {
				playing = false;
				if (!network_cancel) {
					return;
				}
				break;
			}
			if (receive_error == AVERROR(EAGAIN)) {
				if (!packet_pending && !demux_eof) {
					if (network_cancel && presentation_frame->format >= 0) {
						// Publish the newest usable frame before a packet read
						// can wait for the next network segment. Keep that read
						// for the next worker update instead of hiding a frame
						// that already belongs on the consumed-audio timeline.
						break;
					}
					const int read_error =
#ifdef MODULE_MEDIA_STREAM_ENABLED
							packet_reader.is_started() ? packet_reader.read(packet) :
#endif
													   av_read_frame(format, packet);
					packet_waiting = read_error == AVERROR(EAGAIN);
					if (read_error == AVERROR(EAGAIN)) {
						if (!network_cancel) {
							return;
						}
						break;
					}
					if (read_error < 0) {
						if (network_cancel && read_error != AVERROR_EOF) {
							stream_error = ERR_CONNECTION_ERROR;
							playing = false;
							return;
						}
						demux_eof = true;
					} else if (packet->stream_index != video_stream) {
						av_packet_unref(packet);
						continue;
					} else {
						if (network_cancel && packet->dts != AV_NOPTS_VALUE && last_packet_dts != AV_NOPTS_VALUE) {
							// libavformat exposes discontinuous source DTS. We
							// cannot infer a shared epoch for separate inputs.
							// Reject a reset instead of dropping frames forever
							// or silently stitching unrelated audio/video clocks.
							const double previous = double(last_packet_dts) * av_q2d(video_time_base);
							const double current = double(packet->dts) * av_q2d(video_time_base);
							if (current < previous - 1.0) {
								stream_error = ERR_UNAVAILABLE;
								stream_error_message = vformat("The source timestamps changed by %.3f seconds; this media discontinuity is not supported.", current - previous);
								playing = false;
								av_packet_unref(packet);
								return;
							}
						}
						if (packet->dts != AV_NOPTS_VALUE) {
							last_packet_dts = packet->dts;
						}
						packet_pending = true;
					}
				}
				if (packet_pending && waiting_for_keyframe) {
					if (!(packet->flags & AV_PKT_FLAG_KEY)) {
						av_packet_unref(packet);
						packet_pending = false;
						continue;
					}
					waiting_for_keyframe = false;
				}
				if (decoder_draining) {
					if (!network_cancel) {
						return;
					}
					break;
				}
				int send_error;
				{
#ifdef DEV_ENABLED
					DebugMeasure send_measure(debug_send_time);
#endif
					send_error = avcodec_send_packet(codec, packet_pending ? packet : nullptr);
				}
				if (send_error == AVERROR(EAGAIN)) {
					continue;
				}
				if (send_error < 0) {
					if (_fallback_to_software("hardware packet decoding failed")) {
						return;
					}
					ERR_PRINT("FFmpeg: video packet decoding failed.");
					stream_error = ERR_INVALID_DATA;
					playing = false;
					return;
				}
				decoder_draining = !packet_pending;
				av_packet_unref(packet);
				packet_pending = false;
				continue;
			}
			if (receive_error < 0) {
				if (_fallback_to_software("hardware frame decoding failed")) {
					return;
				}
				ERR_PRINT("FFmpeg: video frame decoding failed.");
				stream_error = ERR_INVALID_DATA;
				playing = false;
				return;
			}
			frame_position = frame->best_effort_timestamp == AV_NOPTS_VALUE ? position : frame->best_effort_timestamp * av_q2d(video_time_base) - timestamp_origin;
#ifdef DEV_ENABLED
			debug_last_frame_position = frame_position;
			debug_decoded_frames++;
#endif
			target_clock = _get_target_clock() + lead;
			if (network_cancel && frame_position < 0.0) {
#ifdef DEV_ENABLED
				debug_negative_frames++;
#endif
				// A live video input can precede its paired audio's source
				// origin. Decode its reference frames without publishing a
				// negative playback position or displaying stale content.
				av_frame_unref(frame);
				continue;
			}
			if (seek_preroll && frame_position < seek_target) {
				av_frame_unref(frame);
				continue;
			}
			const bool first_seek_frame = seek_preroll;
			seek_preroll = false;
			// The asynchronous frontend needs the first live CPU image as its
			// prepared poster, even when its timestamp is just after the origin.
			// Only subsequent frames wait for the shared presentation clock.
			if ((!first_seek_frame || (preparing_live_start && !async_frames)) && frame_position > target_clock) {
				av_frame_ref(queued_frame, frame);
				if (!network_cancel) {
					return;
				}
				break;
			}
			frame_ready = true;
			if (network_cancel) {
#ifdef DEV_ENABLED
				debug_late_discards += presentation_frame->format >= 0;
#endif
				av_frame_unref(presentation_frame);
				av_frame_ref(presentation_frame, frame);
			}
		}
		if (network_cancel && presentation_frame->format >= 0) {
			// Keep the newest frame at/before the audio clock even when the
			// decoder has already returned a future frame or hit its budget.
			av_frame_unref(frame);
			av_frame_move_ref(frame, presentation_frame);
			frame_position = frame->best_effort_timestamp == AV_NOPTS_VALUE ? position : frame->best_effort_timestamp * av_q2d(video_time_base) - timestamp_origin;
			frame_ready = true;
		}
		if (!frame_ready) {
			return;
		}
#ifdef DEV_ENABLED
		// Exercise late hardware failure without disabling the user's GPU.
		if (hardware_decoding && test_hardware_failure && position >= 0.2) {
			test_hardware_failure = false;
			if (!_fallback_to_software("injected hardware decoder failure")) {
				playing = false;
			} else {
				print_line("FFmpeg: injected hardware decoder failure recovered with software decoding.");
			}
			return;
		}
#endif
		AVFrame *display_frame = frame;
		if (_is_hardware_format(static_cast<AVPixelFormat>(frame->format))) {
			av_frame_unref(software_frame);
			int transfer_error;
			{
#ifdef DEV_ENABLED
				DebugMeasure transfer_measure(debug_transfer_time);
#endif
				transfer_error = av_hwframe_transfer_data(software_frame, frame, 0);
			}
			if (transfer_error < 0) {
				if (_fallback_to_software("hardware frame transfer failed")) {
					return;
				}
				playing = false;
				stream_error = ERR_INVALID_DATA;
				return;
			}
			display_frame = software_frame;
		}
#ifdef DEV_ENABLED
		DebugMeasure convert_measure(debug_convert_time);
#endif
		const int width = display_frame->width;
		const int height = display_frame->height;
		const int64_t pixels = int64_t(width) * height;
		if (width <= 0 || height <= 0 || pixels > INT32_MAX / 4 || (network_cancel && pixels > 16777216)) {
			stream_error = ERR_INVALID_DATA;
			playing = false;
			return;
		}
		// swscale can write complete SIMD blocks past a tightly packed row.
		// Use FFmpeg's aligned, padded allocation rather than the Godot image
		// vector, and keep the full data/linesize arrays required by sws_scale.
		if (rgba_frame->width != width || rgba_frame->height != height) {
			av_frame_unref(rgba_frame);
			rgba_frame->format = AV_PIX_FMT_RGBA;
			rgba_frame->width = width;
			rgba_frame->height = height;
			if (av_frame_get_buffer(rgba_frame, 32) < 0) {
				stream_error = ERR_OUT_OF_MEMORY;
				playing = false;
				return;
			}
		}
		if (av_frame_make_writable(rgba_frame) < 0) {
			stream_error = ERR_OUT_OF_MEMORY;
			playing = false;
			return;
		}
		scale = sws_getCachedContext(scale, width, height, static_cast<AVPixelFormat>(display_frame->format), width, height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!scale) {
			stream_error = ERR_OUT_OF_MEMORY;
			playing = false;
			return;
		}
		Vector<uint8_t> data;
		if (data.resize(int(pixels * 4)) != OK) {
			stream_error = ERR_OUT_OF_MEMORY;
			playing = false;
			return;
		}
		if (sws_scale(scale, display_frame->data, display_frame->linesize, 0, height, rgba_frame->data, rgba_frame->linesize) != height) {
			stream_error = ERR_INVALID_DATA;
			playing = false;
			return;
		}
		uint8_t *packed = data.ptrw();
		for (int y = 0; y < height; y++) {
			memcpy(packed + int64_t(y) * width * 4, rgba_frame->data[0] + int64_t(y) * rgba_frame->linesize[0], width * 4);
		}
		Ref<Image> image = Image::create_from_data(width, height, false, Image::FORMAT_RGBA8, data);
#ifdef DEV_ENABLED
		convert_measure.stop();
		DebugMeasure texture_measure(debug_texture_time);
#endif
		if (async_frames) {
			decoded_image = image;
		} else if (texture.is_valid() && texture->get_width() == width && texture->get_height() == height) {
			texture->update(image);
		} else {
			texture = ImageTexture::create_from_image(image);
		}
		position = frame_position;
		presented_frames++;
#ifdef DEV_ENABLED
		if (debug_first_presented_position < 0.0) {
			debug_first_presented_position = frame_position;
		}
#endif
	}

	void play() override {
		if (live_start_prepared) {
			// Initial live preroll has already selected the first source frame
			// and trimmed audio. A new seek would reopen/change the live edge.
			live_start_prepared = false;
			playing = true;
			playback_clock = 0.0;
			if (audio_playback.is_valid()) {
				audio_playback->start_playback(0.0);
			}
			return;
		}
		// A newly opened live input need not support seeking to its beginning.
		if (!network_cancel || position > 0) {
			seek(0.0);
		}
		playing = true;
		playback_clock = position;
		if (audio_playback.is_valid()) {
			audio_playback->start_playback(position);
		}
	}
	void stop() override {
		playing = false;
		if (audio_playback.is_valid()) {
			audio_playback->stop_playback();
		}
		seek(0.0);
	}
	bool is_playing() const override { return playing; }
	void set_paused(bool p_paused) override {
		// update() is paused too; preserve the audio cursor and rejected samples.
		paused = p_paused;
	}
	bool is_paused() const override { return paused; }
	double get_length() const override { return length; }
	double get_playback_position() const override { return position; }
	void seek(double p_time) override {
#ifdef MODULE_MEDIA_STREAM_ENABLED
		if (packet_reader.is_started()) {
			// Public asynchronous seeks reopen a fresh generation. Never seek
			// a demuxer while its packet producer owns it.
			stream_error = ERR_UNAVAILABLE;
			return;
		}
#endif
		if (format && codec && packet && queued_frame && video_stream >= 0) {
			if (av_seek_frame(format, video_stream, int64_t((p_time + timestamp_origin) / av_q2d(video_time_base)), AVSEEK_FLAG_BACKWARD) < 0) {
				stream_error = ERR_UNAVAILABLE;
				return;
			}
			avcodec_flush_buffers(codec);
			last_packet_dts = AV_NOPTS_VALUE;
			av_frame_unref(queued_frame);
			av_frame_unref(presentation_frame);
			av_packet_unref(packet);
			packet_pending = false;
			demux_eof = false;
			decoder_draining = false;
			waiting_for_keyframe = false;
			pending_audio.clear();
			pending_audio_offset = 0;
			audio_frames_due = 0.0;
			if (audio_playback.is_valid()) {
				// A shorter soundtrack may already have reached EOF. A seek in
				// an active video must reactivate it, without unpausing output.
				if (playing) {
					audio_playback->start_playback(p_time);
				} else {
					audio_playback->seek(p_time);
				}
				if (network_cancel && int(audio_playback->get_parameter(SNAME("stream_error"))) != OK) {
					stream_error = ERR_UNAVAILABLE;
					return;
				}
			}
			position = p_time;
			playback_clock = p_time;
			if (network_cancel) {
				// A demuxer seeks to a preceding keyframe. Decode/discard that
				// preroll on the worker before publishing readiness, including
				// when the caller is paused. Audio has not advanced yet.
				seek_target = p_time;
				seek_preroll = true;
				const bool was_playing = playing;
				const bool was_paused = paused;
				playing = true;
				paused = false;
				const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 30000000;
				while (seek_preroll && playing && stream_error == OK && !network_cancel->is_set() && OS::get_singleton()->get_ticks_usec() < deadline) {
					update(0.0);
				}
				paused = was_paused;
				playing = was_playing;
				if (seek_preroll && stream_error == OK) {
					stream_error = ERR_UNAVAILABLE;
				}
			}
		}
	}
	Ref<Texture2D> get_texture() const override { return texture; }
	int get_channels() const override { return audio_playback.is_valid() ? 2 : 0; }
	int get_mix_rate() const override { return audio_playback.is_valid() ? int(AudioServer::get_singleton()->get_mix_rate()) : 0; }
	void set_audio_playback_position(double p_position) override {
		if (network_cancel) {
			audio_position = p_position;
			audio_clock_available = true;
		}
	}
	void set_audio_playback_position_callback(AudioPlaybackPositionCallback p_callback, void *p_userdata) override {
		audio_clock_callback = p_callback;
		audio_clock_userdata = p_userdata;
	}
};

Ref<VideoStreamPlayback> VideoStreamMedia::instantiate_playback() {
#ifdef MODULE_MEDIA_STREAM_ENABLED
	if (FFmpegFileIO::is_remote(get_file())) {
		return ffmpeg_async_video(get_file(), get_http_headers(), get_audio_file(), get_audio_http_headers(), is_live_dvr_enabled());
	}
#endif
	Ref<VideoStreamPlaybackFFmpeg> playback;
	playback.instantiate();
	if (!playback->open_file(get_file())) {
		return Ref<VideoStreamPlayback>();
	}
	return playback;
}

Ref<VideoStreamPlayback> ffmpeg_open_video_on_worker(const String &p_path, const Dictionary &p_headers, const SafeFlag *p_cancel, const String &p_audio_file, const Dictionary &p_audio_headers, String *r_error, FFmpegDVRWindow *p_dvr) {
	Ref<VideoStreamPlaybackFFmpeg> playback;
	playback.instantiate();
	playback->use_async_frames();
	return playback->open_file(p_path, p_headers, p_cancel, p_audio_file, p_audio_headers, r_error, p_dvr) ? Ref<VideoStreamPlayback>(playback) : Ref<VideoStreamPlayback>();
}

bool ffmpeg_start_video_read_ahead(const Ref<VideoStreamPlayback> &p_playback, String *r_error) {
	Ref<VideoStreamPlaybackFFmpeg> playback = p_playback;
	if (playback.is_null()) {
		if (r_error) {
			*r_error = "The native video playback is unavailable.";
		}
		return false;
	}
	return playback->start_read_ahead(r_error);
}

void VideoStreamMedia::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_live_dvr_enabled", "enabled"), &VideoStreamMedia::set_live_dvr_enabled);
	ClassDB::bind_method(D_METHOD("is_live_dvr_enabled"), &VideoStreamMedia::is_live_dvr_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "live_dvr_enabled"), "set_live_dvr_enabled", "is_live_dvr_enabled");
#ifdef MODULE_MEDIA_STREAM_ENABLED
	ClassDB::bind_method(D_METHOD("set_http_headers", "headers"), &VideoStreamMedia::set_http_headers);
	ClassDB::bind_method(D_METHOD("get_http_headers"), &VideoStreamMedia::get_http_headers);
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "http_headers"), "set_http_headers", "get_http_headers");
	ClassDB::bind_method(D_METHOD("set_audio_file", "file"), &VideoStreamMedia::set_audio_file);
	ClassDB::bind_method(D_METHOD("get_audio_file"), &VideoStreamMedia::get_audio_file);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "audio_file"), "set_audio_file", "get_audio_file");
	ClassDB::bind_method(D_METHOD("set_audio_http_headers", "headers"), &VideoStreamMedia::set_audio_http_headers);
	ClassDB::bind_method(D_METHOD("get_audio_http_headers"), &VideoStreamMedia::get_audio_http_headers);
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "audio_http_headers"), "set_audio_http_headers", "get_audio_http_headers");
#endif
}

Ref<Resource> ResourceFormatLoaderMedia::load(const String &p_path, const String &, Error *r_error, bool, float *, CacheMode) {
	String extension = p_path.get_extension().to_lower();
	Ref<Resource> stream;
	if (extension == "mp4" || extension == "m4v" || extension == "mov" || extension == "mkv" || extension == "webm" || extension == "ogv") {
		Ref<VideoStreamMedia> video_stream;
		video_stream.instantiate();
		video_stream->set_file(p_path);
		stream = video_stream;
	} else {
		Ref<AudioStreamMedia> audio_stream;
		audio_stream.instantiate();
		audio_stream->set_file(p_path);
		stream = audio_stream;
	}
	if (r_error) {
		*r_error = OK;
	}
	return stream;
}

void ResourceFormatLoaderMedia::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("mp4");
	p_extensions->push_back("m4v");
	p_extensions->push_back("mov");
	p_extensions->push_back("mkv");
	p_extensions->push_back("webm");
	p_extensions->push_back("ogv");
	p_extensions->push_back("mp3");
	p_extensions->push_back("m4a");
	p_extensions->push_back("aac");
	p_extensions->push_back("flac");
	p_extensions->push_back("ogg");
	p_extensions->push_back("opus");
	p_extensions->push_back("wav");
}

static bool _media_has_imported_resource(const String &p_path) {
	ResourceFormatImporter *importer = ResourceFormatImporter::get_singleton();
	// Keep/Skip sidecars have no imported resource type; raw loading still applies.
	return importer && FileAccess::exists(p_path + ".import") && !importer->get_resource_type(p_path).is_empty();
}

bool ResourceFormatLoaderMedia::recognize_path(const String &p_path, const String &p_for_type) const {
	// Imported resources may contain the only copy shipped in an exported PCK.
	// Let ResourceFormatImporter load them instead of opening the missing source.
	return ResourceFormatLoader::recognize_path(p_path, p_for_type) && !_media_has_imported_resource(p_path);
}

bool ResourceFormatLoaderMedia::handles_type(const String &p_type) const {
	return p_type == "VideoStreamMedia" || p_type == "AudioStreamMedia";
}
String ResourceFormatLoaderMedia::get_resource_type(const String &p_path) const {
	String extension = p_path.get_extension().to_lower();
	if (extension == "mp4" || extension == "m4v" || extension == "mov" || extension == "mkv" || extension == "webm" || extension == "ogv") {
		return _media_has_imported_resource(p_path) ? String() : String("VideoStreamMedia");
	}
	if (extension == "mp3" || extension == "m4a" || extension == "aac" || extension == "flac" || extension == "ogg" || extension == "opus" || extension == "wav") {
		return _media_has_imported_resource(p_path) ? String() : String("AudioStreamMedia");
	}
	return String();
}
