#include "video_stream_media.h"

#include "core/error/error_macros.h"
#include "core/io/file_access.h"
#include "core/io/resource_importer.h"
#include "core/object/class_db.h"
#include "core/os/memory.h"
#include "core/os/os.h"
#include "ffmpeg_file_io.h"
#include "scene/resources/image_texture.h"
#include "servers/audio/audio_server.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

class VideoStreamPlaybackFFmpeg : public VideoStreamPlayback {
    GDCLASS(VideoStreamPlaybackFFmpeg, VideoStreamPlayback);

    FFmpegFileIO file_io;
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    AVFrame *queued_frame = nullptr;
    AVFrame *software_frame = nullptr;
    AVBufferRef *hardware_device = nullptr;
    SwsContext *scale = nullptr;
    int video_stream = -1;
    bool playing = false;
    bool paused = false;
    double position = 0.0;
    double length = 0.0;
    double playback_clock = 0.0;
    double audio_frames_due = 0.0;
    Vector<float> pending_audio;
    int pending_audio_offset = 0;
    bool packet_pending = false;
    bool demux_eof = false;
    bool decoder_draining = false;
    bool hardware_decoding = false;
#ifdef DEV_ENABLED
    bool test_hardware_failure = false;
#endif
    Ref<ImageTexture> texture;
    Ref<AudioStreamPlayback> audio_playback;

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
        if (avcodec_parameters_to_context(candidate, format->streams[video_stream]->codecpar) < 0) {
            avcodec_free_context(&candidate);
            return false;
        }
        candidate->pkt_timebase = format->streams[video_stream]->time_base;
        candidate->get_format = p_device ? _get_format : (p_hardware ? avcodec_default_get_format : _get_software_format);
#if defined(WEB_ENABLED) && !defined(THREADS_ENABLED)
        candidate->thread_count = 1;
#endif
        if (p_device) {
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
        const AVCodecID id = format->streams[video_stream]->codecpar->codec_id;
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
            AV_HWDEVICE_TYPE_D3D12VA,
            AV_HWDEVICE_TYPE_D3D11VA,
            AV_HWDEVICE_TYPE_DXVA2,
            AV_HWDEVICE_TYPE_CUDA,
            AV_HWDEVICE_TYPE_QSV,
            AV_HWDEVICE_TYPE_AMF,
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

        const AVCodecID id = format->streams[video_stream]->codecpar->codec_id;
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
                    if (av_hwdevice_ctx_create(&device, device_type, nullptr, nullptr, 0) >= 0 && _open_decoder(decoder, device, true)) {
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
        av_frame_unref(software_frame);
        av_packet_unref(packet);
        avcodec_free_context(&codec);
        av_buffer_unref(&hardware_device);
        const AVRational time_base = format->streams[video_stream]->time_base;
        if (av_seek_frame(format, video_stream, int64_t(position / av_q2d(time_base)), AVSEEK_FLAG_BACKWARD) < 0 || !_open_software_decoder()) {
            return false;
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
                audio_frames_due = 0.0;
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
        if (software_frame) {
            av_frame_free(&software_frame);
        }
        if (hardware_device) {
            av_buffer_unref(&hardware_device);
        }
        video_stream = -1;
        playing = false;
        playback_clock = 0.0;
        audio_frames_due = 0.0;
        pending_audio.clear();
        pending_audio_offset = 0;
        packet_pending = false;
        demux_eof = false;
        decoder_draining = false;
        hardware_decoding = false;
        audio_playback.unref();
    }

public:
    ~VideoStreamPlaybackFFmpeg() override { close(); }

    bool open_file(const String &p_path) {
        close();
        if (file_io.open(&format, p_path) < 0 || avformat_find_stream_info(format, nullptr) < 0) {
            close();
            return false;
        }
        video_stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (video_stream < 0) {
            close();
            return false;
        }
        const bool force_software = OS::get_singleton()->get_environment("GODOT_FFMPEG_FORCE_SOFTWARE") == "1";
#ifdef DEV_ENABLED
        test_hardware_failure = OS::get_singleton()->get_environment("GODOT_FFMPEG_TEST_HW_FAILURE") == "1";
#endif
        if ((force_software || !_try_open_hardware_decoder()) && !_open_software_decoder()) {
            close();
            return false;
        }
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        queued_frame = av_frame_alloc();
        software_frame = av_frame_alloc();
        if (packet && frame && queued_frame) {
            Ref<Image> image = Image::create_empty(codec->width, codec->height, false, Image::FORMAT_RGBA8);
            texture = ImageTexture::create_from_image(image);
        }
        audio_playback = ffmpeg_audio_playback_from_buffer(Vector<uint8_t>(), nullptr, p_path);
        length = format->duration > 0 ? double(format->duration) / AV_TIME_BASE : 0.0;
        return packet && frame && queued_frame && software_frame;
    }

    void update(double p_delta) override {
        if (!playing || paused || !format || !codec) {
            return;
        }
        playback_clock += MAX(p_delta, 0.0);
        _mix_audio(p_delta);
        const double target_clock = playback_clock - (audio_playback.is_valid() ? audio_frames_due / AudioServer::get_singleton()->get_mix_rate() : 0.0);
        const uint64_t decode_start = OS::get_singleton()->get_ticks_usec();
        bool frame_ready = false;
        double frame_position = position;
        if (queued_frame->format >= 0) {
            const double queued_position = queued_frame->best_effort_timestamp == AV_NOPTS_VALUE ? position : queued_frame->best_effort_timestamp * av_q2d(format->streams[video_stream]->time_base);
            if (queued_position > target_clock) {
                return;
            }
            av_frame_unref(frame);
            av_frame_move_ref(frame, queued_frame);
            frame_position = queued_position;
            frame_ready = true;
        }
        while (!frame_ready || frame_position < target_clock - 0.1) {
            // Decode reference frames, but avoid uploading stale frames. Limit catch-up work.
            if (OS::get_singleton()->get_ticks_usec() - decode_start >= 8000) {
                break;
            }
            if (frame_ready) {
                av_frame_unref(frame);
                frame_ready = false;
            }
            const int receive_error = avcodec_receive_frame(codec, frame);
            if (receive_error == AVERROR_EOF) {
                playing = false;
                return;
            }
            if (receive_error == AVERROR(EAGAIN)) {
                if (!packet_pending && !demux_eof) {
                    const int read_error = av_read_frame(format, packet);
                    if (read_error == AVERROR(EAGAIN)) {
                        return;
                    }
                    if (read_error < 0) {
                        demux_eof = true;
                    } else if (packet->stream_index != video_stream) {
                        av_packet_unref(packet);
                        continue;
                    } else {
                        packet_pending = true;
                    }
                }
                if (decoder_draining) {
                    return;
                }
                const int send_error = avcodec_send_packet(codec, packet_pending ? packet : nullptr);
                if (send_error == AVERROR(EAGAIN)) {
                    continue;
                }
                if (send_error < 0) {
                    if (_fallback_to_software("hardware packet decoding failed")) {
                        return;
                    }
                    ERR_PRINT("FFmpeg: video packet decoding failed.");
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
                playing = false;
                return;
            }
            frame_position = frame->best_effort_timestamp == AV_NOPTS_VALUE ? position : frame->best_effort_timestamp * av_q2d(format->streams[video_stream]->time_base);
            if (frame_position > target_clock) {
                av_frame_ref(queued_frame, frame);
                return;
            }
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
            }
            return;
        }
#endif
        AVFrame *display_frame = frame;
        if (_is_hardware_format(static_cast<AVPixelFormat>(frame->format))) {
            av_frame_unref(software_frame);
            if (av_hwframe_transfer_data(software_frame, frame, 0) < 0) {
                if (_fallback_to_software("hardware frame transfer failed")) {
                    return;
                }
                playing = false;
                return;
            }
            display_frame = software_frame;
        }
        scale = sws_getCachedContext(scale, codec->width, codec->height, static_cast<AVPixelFormat>(display_frame->format), codec->width, codec->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scale) {
            playing = false;
            return;
        }
        Vector<uint8_t> data;
        data.resize(codec->width * codec->height * 4);
        uint8_t *dst[] = { data.ptrw() };
        int stride[] = { codec->width * 4 };
        sws_scale(scale, display_frame->data, display_frame->linesize, 0, codec->height, dst, stride);
        Ref<Image> image = Image::create_from_data(codec->width, codec->height, false, Image::FORMAT_RGBA8, data);
        if (texture.is_valid()) {
            texture->update(image);
        } else {
            texture = ImageTexture::create_from_image(image);
        }
        position = frame_position;
    }

    void play() override {
        seek(0.0);
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
        if (format && codec && packet && queued_frame && video_stream >= 0) {
            if (av_seek_frame(format, video_stream, int64_t(p_time / av_q2d(format->streams[video_stream]->time_base)), AVSEEK_FLAG_BACKWARD) < 0) {
                return;
            }
            avcodec_flush_buffers(codec);
            av_frame_unref(queued_frame);
            av_packet_unref(packet);
            packet_pending = false;
            demux_eof = false;
            decoder_draining = false;
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
            }
            position = p_time;
            playback_clock = p_time;
        }
    }
    Ref<Texture2D> get_texture() const override { return texture; }
    int get_channels() const override { return audio_playback.is_valid() ? 2 : 0; }
    int get_mix_rate() const override { return audio_playback.is_valid() ? int(AudioServer::get_singleton()->get_mix_rate()) : 0; }
};

Ref<VideoStreamPlayback> VideoStreamMedia::instantiate_playback() {
    Ref<VideoStreamPlaybackFFmpeg> playback;
    playback.instantiate();
    if (!playback->open_file(get_file())) {
        return Ref<VideoStreamPlayback>();
    }
    return playback;
}

void VideoStreamMedia::_bind_methods() {}

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

bool ResourceFormatLoaderMedia::handles_type(const String &p_type) const { return p_type == "VideoStreamMedia" || p_type == "AudioStreamMedia"; }
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
