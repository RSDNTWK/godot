#include "video_stream_media.h"

#include "core/error/error_macros.h"
#include "core/io/file_access.h"
#include "core/object/class_db.h"
#include "core/os/memory.h"
#include "core/os/os.h"
#include "core/config/project_settings.h"
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
    double position = 0.0;
    double length = 0.0;
    double playback_clock = 0.0;
    double audio_frames_due = 0.0;
    Vector<float> pending_audio;
    int pending_audio_offset = 0;
    bool packet_pending = false;
    bool demux_eof = false;
    bool decoder_draining = false;
    Ref<ImageTexture> texture;
    Ref<AudioStreamPlayback> audio_playback;

    static bool _is_hardware_format(AVPixelFormat p_format) {
        const AVPixFmtDescriptor *descriptor = av_pix_fmt_desc_get(p_format);
        return descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL);
    }

    static bool _try_create_hardware_device(AVBufferRef **r_device) {
#if defined(WINDOWS_ENABLED) || defined(ANDROID_ENABLED) || defined(LINUXBSD_ENABLED) || defined(MACOS_ENABLED) || defined(APPLE_EMBEDDED_ENABLED)
        const AVHWDeviceType device_types[] = {
#if defined(WINDOWS_ENABLED)
            AV_HWDEVICE_TYPE_D3D12VA,
            AV_HWDEVICE_TYPE_D3D11VA,
            AV_HWDEVICE_TYPE_DXVA2,
            AV_HWDEVICE_TYPE_CUDA,
            AV_HWDEVICE_TYPE_QSV,
#elif defined(ANDROID_ENABLED)
            AV_HWDEVICE_TYPE_MEDIACODEC,
#elif defined(LINUXBSD_ENABLED)
            AV_HWDEVICE_TYPE_CUDA,
            AV_HWDEVICE_TYPE_QSV,
            AV_HWDEVICE_TYPE_VAAPI,
            AV_HWDEVICE_TYPE_VULKAN,
#elif defined(MACOS_ENABLED) || defined(APPLE_EMBEDDED_ENABLED)
            AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#endif
        };

        for (AVHWDeviceType device_type : device_types) {
            if (av_hwdevice_ctx_create(r_device, device_type, nullptr, nullptr, 0) >= 0) {
                print_verbose(vformat("FFmpeg: selected hardware decoder device '%s'.", av_hwdevice_get_type_name(device_type)));
                return true;
            }
        }
#else
        (void)r_device;
#endif
        return false;
    }

    static AVPixelFormat _get_format(AVCodecContext *p_codec, const AVPixelFormat *p_formats) {
        AVHWDeviceType device_type = AV_HWDEVICE_TYPE_NONE;
        if (p_codec->hw_device_ctx) {
            AVHWDeviceContext *device = reinterpret_cast<AVHWDeviceContext *>(p_codec->hw_device_ctx->data);
            device_type = device->type;
        }
        for (const AVPixelFormat *format = p_formats; *format != AV_PIX_FMT_NONE; format++) {
            const bool compatible =
                    (device_type == AV_HWDEVICE_TYPE_D3D12VA && *format == AV_PIX_FMT_D3D12) ||
                    (device_type == AV_HWDEVICE_TYPE_D3D11VA && *format == AV_PIX_FMT_D3D11) ||
                    (device_type == AV_HWDEVICE_TYPE_DXVA2 && *format == AV_PIX_FMT_DXVA2_VLD) ||
                    (device_type == AV_HWDEVICE_TYPE_MEDIACODEC && *format == AV_PIX_FMT_MEDIACODEC) ||
                    (device_type == AV_HWDEVICE_TYPE_VAAPI && *format == AV_PIX_FMT_VAAPI) ||
                    (device_type == AV_HWDEVICE_TYPE_VULKAN && *format == AV_PIX_FMT_VULKAN) ||
                    (device_type == AV_HWDEVICE_TYPE_VIDEOTOOLBOX && *format == AV_PIX_FMT_VIDEOTOOLBOX) ||
                    (device_type == AV_HWDEVICE_TYPE_CUDA && *format == AV_PIX_FMT_CUDA) ||
                    (device_type == AV_HWDEVICE_TYPE_QSV && *format == AV_PIX_FMT_QSV);
            if (compatible) {
                return *format;
            }
        }
        return avcodec_default_get_format(p_codec, p_formats);
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
        audio_playback.unref();
    }

public:
    ~VideoStreamPlaybackFFmpeg() override { close(); }

    bool open_file(const String &p_path) {
        close();
        String path = ProjectSettings::get_singleton()->globalize_path(p_path);
        if (avformat_open_input(&format, path.utf8().get_data(), nullptr, nullptr) < 0 || avformat_find_stream_info(format, nullptr) < 0) {
            close();
            return false;
        }
        video_stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (video_stream < 0) {
            close();
            return false;
        }
        const AVCodecParameters *params = format->streams[video_stream]->codecpar;
        const AVCodec *decoder = avcodec_find_decoder(params->codec_id);
        if (!decoder) {
            close();
            return false;
        }
        codec = avcodec_alloc_context3(decoder);
        if (!codec || avcodec_parameters_to_context(codec, params) < 0) {
            close();
            return false;
        }
        if (_try_create_hardware_device(&hardware_device)) {
            codec->hw_device_ctx = av_buffer_ref(hardware_device);
            codec->get_format = _get_format;
        } else {
            print_verbose("FFmpeg: no compatible hardware decoder device found; using software decoding.");
        }
        int codec_open_error = avcodec_open2(codec, decoder, nullptr);
        if (codec_open_error < 0 && hardware_device) {
            print_verbose("FFmpeg: hardware decoder setup failed; retrying with software decoding.");
            avcodec_free_context(&codec);
            av_buffer_unref(&hardware_device);
            codec = avcodec_alloc_context3(decoder);
            if (codec && avcodec_parameters_to_context(codec, params) >= 0) {
                codec_open_error = avcodec_open2(codec, decoder, nullptr);
            }
        }
        if (codec_open_error < 0) {
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
        audio_playback = ffmpeg_audio_playback_from_buffer(FileAccess::get_file_as_bytes(path));
        length = format->duration > 0 ? double(format->duration) / AV_TIME_BASE : 0.0;
        return packet && frame && queued_frame && software_frame;
    }

    void update(double p_delta) override {
        if (!playing || !format || !codec) {
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
        AVFrame *display_frame = frame;
        if (_is_hardware_format(static_cast<AVPixelFormat>(frame->format))) {
            if (av_hwframe_transfer_data(software_frame, frame, 0) < 0) {
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
        seek(position);
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
    }
    bool is_playing() const override { return playing; }
    void set_paused(bool p_paused) override {
        // update() is paused too; preserve the audio cursor and rejected samples.
        playing = !p_paused;
    }
    bool is_paused() const override { return !playing; }
    double get_length() const override { return length; }
    double get_playback_position() const override { return position; }
    void seek(double p_time) override {
        if (format && video_stream >= 0) {
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
                audio_playback->seek(p_time);
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

bool ResourceFormatLoaderMedia::handles_type(const String &p_type) const { return p_type == "VideoStreamMedia" || p_type == "AudioStreamMedia"; }
String ResourceFormatLoaderMedia::get_resource_type(const String &p_path) const {
	String extension = p_path.get_extension().to_lower();
	if (extension == "mp4" || extension == "m4v" || extension == "mov" || extension == "mkv" || extension == "webm" || extension == "ogv") {
		return "VideoStreamMedia";
	}
	if (extension == "mp3" || extension == "m4a" || extension == "aac" || extension == "flac" || extension == "ogg" || extension == "opus" || extension == "wav") {
		return "AudioStreamMedia";
	}
	return String();
}
