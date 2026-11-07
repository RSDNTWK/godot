#ifndef FFMPEG_FILE_IO_H
#define FFMPEG_FILE_IO_H

#include "core/io/file_access.h"
#include "core/templates/safe_refcount.h"
#include "modules/modules_enabled.gen.h"

#ifdef MODULE_MEDIA_STREAM_ENABLED
#include "ffmpeg_remote_io.h"
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

// FFmpeg must use Godot's virtual filesystem for files stored inside a PCK.
class FFmpegFileIO {
    Ref<FileAccess> file;
    AVIOContext *io = nullptr;
#ifdef MODULE_MEDIA_STREAM_ENABLED
    FFmpegRemoteIO *remote = nullptr;
    Dictionary remote_headers;
    const SafeFlag *remote_cancel = nullptr;
    const SafeFlag *remote_stop = nullptr;
    Ref<FFmpegDVRWindow> dvr;
    bool dvr_primary = true;
#endif

    static int read(void *p_opaque, uint8_t *p_buffer, int p_size) {
        FFmpegFileIO *self = static_cast<FFmpegFileIO *>(p_opaque);
        const uint64_t count = self->file->get_buffer(p_buffer, p_size);
        if (count > 0) {
            return int(count);
        }
        return self->file->get_position() >= self->file->get_length() ? AVERROR_EOF : AVERROR(EIO);
    }

    static int64_t seek(void *p_opaque, int64_t p_offset, int p_whence) {
        FFmpegFileIO *self = static_cast<FFmpegFileIO *>(p_opaque);
        const int64_t size = int64_t(self->file->get_length());
        if (p_whence & AVSEEK_SIZE) {
            return size;
        }
        p_whence &= ~AVSEEK_FORCE;
        int64_t base = 0;
        if (p_whence == SEEK_CUR) {
            base = int64_t(self->file->get_position());
        } else if (p_whence == SEEK_END) {
            base = size;
        } else if (p_whence != SEEK_SET) {
            return AVERROR(EINVAL);
        }
        if (p_offset < -base || p_offset > size - base) {
            return AVERROR(EINVAL);
        }
        self->file->seek(base + p_offset);
        if (self->file->get_position() != uint64_t(base + p_offset)) {
            return AVERROR(EIO);
        }
        return base + p_offset;
    }

public:
#ifdef MODULE_MEDIA_STREAM_ENABLED
    void set_dvr(const Ref<FFmpegDVRWindow> &p_dvr, bool p_primary) { dvr = p_dvr; dvr_primary = p_primary; }
    double get_dvr_input_start() const { return remote ? remote->get_dvr_input_start() : -1; }
#endif
    static bool is_remote(const String &p_path) {
        return p_path.contains("://") && !p_path.begins_with("res://") && !p_path.begins_with("user://") && !p_path.begins_with("uid://");
    }
    void set_remote_options(const Dictionary &p_headers, const SafeFlag *p_cancel, const SafeFlag *p_stop = nullptr) {
#ifdef MODULE_MEDIA_STREAM_ENABLED
        remote_headers = p_headers;
        remote_cancel = p_cancel;
        remote_stop = p_stop;
#endif
    }
    // Close the AVFormatContext before releasing its custom IO.
    void close() {
#ifdef MODULE_MEDIA_STREAM_ENABLED
        if (remote) {
            memdelete(remote);
            remote = nullptr;
        }
#endif
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
        file.unref();
    }

    int open(AVFormatContext **r_format, const String &p_path, const AVInputFormat *p_input_format = nullptr) {
        close();
        // Keep FFmpeg protocol handling for remote URLs.
        if (is_remote(p_path)) {
#ifdef MODULE_MEDIA_STREAM_ENABLED
            if (!MediaHTTP::valid_url(p_path) || !remote_cancel) {
                return AVERROR(EACCES);
            }
            remote = memnew(FFmpegRemoteIO(remote_cancel, remote_headers, remote_stop));
            remote->set_dvr(dvr, dvr_primary);
            return remote->open_format(r_format, p_path, p_input_format);
#else
            return avformat_open_input(r_format, p_path.utf8().get_data(), p_input_format, nullptr);
#endif
        }
        file = FileAccess::open(p_path, FileAccess::READ);
        if (file.is_null()) {
            return AVERROR(ENOENT);
        }
        if (file->get_length() > uint64_t(INT64_MAX)) {
            close();
            return AVERROR(EINVAL);
        }
        uint8_t *buffer = static_cast<uint8_t *>(av_malloc(32768));
        if (!buffer) {
            close();
            return AVERROR(ENOMEM);
        }
        io = avio_alloc_context(buffer, 32768, 0, this, read, nullptr, seek);
        if (!io) {
            av_free(buffer);
            close();
            return AVERROR(ENOMEM);
        }
        *r_format = avformat_alloc_context();
        if (!*r_format) {
            close();
            return AVERROR(ENOMEM);
        }
        io->seekable = AVIO_SEEKABLE_NORMAL;
        (*r_format)->pb = io;
        (*r_format)->flags |= AVFMT_FLAG_CUSTOM_IO;
        return avformat_open_input(r_format, p_path.utf8().get_data(), p_input_format, nullptr);
    }

    ~FFmpegFileIO() { close(); }
};

#endif
