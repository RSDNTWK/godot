#ifndef FFMPEG_FILE_IO_H
#define FFMPEG_FILE_IO_H

#include "core/io/file_access.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

// A seek may build a demuxer index internally. Its IO must share the
// caller's work allowance; EAGAIN cannot cancel generic seeking (it retries it).
struct FFmpegIOWorkBudget {
	int64_t &remaining;
	int64_t &bytes_remaining;
	bool exhausted = false;
	FFmpegIOWorkBudget(int64_t &p_remaining, int64_t &p_bytes) : remaining(p_remaining), bytes_remaining(p_bytes) {}
	bool step() {
		if (remaining <= 0) {
			exhausted = true;
			return false;
		}
		remaining--;
		return true;
	}
	int read_size(int p_requested) {
		if (bytes_remaining <= 0 || !step()) {
			exhausted = true;
			return 0;
		}
		const int amount = int(MIN(int64_t(p_requested), bytes_remaining));
		bytes_remaining -= amount;
		return amount;
	}
};

// FFmpeg must use Godot's virtual filesystem for files stored inside a PCK.
class FFmpegFileIO {
	Ref<FileAccess> file;
	AVIOContext *io = nullptr;
	FFmpegIOWorkBudget *seek_budget = nullptr;

	static int reject_external_resource(AVFormatContext *, AVIOContext **r_io, const char *, int, AVDictionary **) {
		// Supported inputs are self-contained and use the custom primary IO.
		// A file-protocol path can itself name a network mount, so do not open
		// secondary resources even when their text looks like a local filename.
		*r_io = nullptr;
		return AVERROR_PROTOCOL_NOT_FOUND;
	}

	static int read(void *p_opaque, uint8_t *p_buffer, int p_size) {
		FFmpegFileIO *self = static_cast<FFmpegFileIO *>(p_opaque);
		if (self->seek_budget) {
			p_size = self->seek_budget->read_size(p_size);
			if (p_size <= 0) {
				return AVERROR_EXIT;
			}
		}
		const uint64_t count = self->file->get_buffer(p_buffer, p_size);
		if (count > 0) {
			return int(count);
		}
		return self->file->get_position() >= self->file->get_length() ? AVERROR_EOF : AVERROR(EIO);
	}

	static int64_t seek(void *p_opaque, int64_t p_offset, int p_whence) {
		FFmpegFileIO *self = static_cast<FFmpegFileIO *>(p_opaque);
		if (self->seek_budget && !self->seek_budget->step()) {
			return AVERROR_EXIT;
		}
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
	static bool is_remote_path(const String &p_path) {
		if (p_path.begins_with("res://") || p_path.begins_with("user://") || p_path.begins_with("uid://")) { return false; }
		const auto slash = [](char32_t p_char) { return p_char == '/' || p_char == '\\'; };
		if (p_path.length() >= 2 && slash(p_path[0]) && slash(p_path[1])) { return true; }
		const int colon = p_path.find(":");
		if (colon <= 0) { return false; }
		const auto alpha = [](char32_t p_char) { return (p_char >= 'a' && p_char <= 'z') || (p_char >= 'A' && p_char <= 'Z'); };
		// Native drive paths are local paths, rather than URI schemes.
		if (colon == 1 && alpha(p_path[0]) && p_path.length() > 2 && (p_path[2] == '/' || p_path[2] == '\\')) { return false; }
		if (!alpha(p_path[0])) { return false; }
		for (int i = 1; i < colon; i++) {
			const char32_t c = p_path[i];
			if (!alpha(c) && !(c >= '0' && c <= '9') && c != '+' && c != '-' && c != '.') { return false; }
		}
		return true;
	}

	// The application downloads remote media asynchronously before playback.
	// Restrict memory inputs too: demuxers may open secondary URLs.
	static int configure_local_input(AVFormatContext *p_format) {
		if (!p_format) { return AVERROR(ENOMEM); }
		av_freep(&p_format->protocol_whitelist);
		p_format->protocol_whitelist = av_strdup("file");
		p_format->io_open = reject_external_resource;
		return p_format->protocol_whitelist ? 0 : AVERROR(ENOMEM);
	}

	void set_seek_budget(FFmpegIOWorkBudget *p_budget) { seek_budget = p_budget; }
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
		if (is_remote_path(p_path)) {
			return AVERROR_PROTOCOL_NOT_FOUND;
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
		const int configured = configure_local_input(*r_format);
		if (configured < 0) {
			avformat_free_context(*r_format);
			*r_format = nullptr;
			close();
			return configured;
		}
		io->seekable = AVIO_SEEKABLE_NORMAL;
		(*r_format)->pb = io;
		(*r_format)->flags |= AVFMT_FLAG_CUSTOM_IO;
		return avformat_open_input(r_format, p_path.utf8().get_data(), p_input_format, nullptr);
	}

	~FFmpegFileIO() { close(); }
};

#endif
