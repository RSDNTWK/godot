#ifndef FFMPEG_FILE_IO_H
#define FFMPEG_FILE_IO_H

#include "core/io/file_access.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

// FFmpeg must use Godot's virtual filesystem for files stored inside a PCK.
class FFmpegFileIO {
    Ref<FileAccess> file;
    AVIOContext *io = nullptr;

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
    // Close the AVFormatContext before releasing its custom IO.
    void close() {
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
        file.unref();
    }

    int open(AVFormatContext **r_format, const String &p_path, const AVInputFormat *p_input_format = nullptr) {
        close();
        // Keep FFmpeg protocol handling for remote URLs.
        if (p_path.contains("://") && !p_path.begins_with("res://") && !p_path.begins_with("user://") && !p_path.begins_with("uid://")) {
            return avformat_open_input(r_format, p_path.utf8().get_data(), p_input_format, nullptr);
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