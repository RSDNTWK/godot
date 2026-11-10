/**************************************************************************/
/*  ffmpeg_remote_io.h                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "ffmpeg_dvr.h"

#include "core/os/mutex.h"
#include "core/os/os.h"

#include "modules/media_stream/media_http.h"

#include <memory>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

class FFmpegRemoteIO {
	// Unit tests exercise FFmpeg's real secondary-open callback without sockets.
	friend struct FFmpegRemoteIOTestAccess;
	MediaHTTP http;
	const SafeFlag *cancel;
	const SafeFlag *stop;
	uint64_t absolute_deadline = 0;
	Dictionary headers;
	AVIOContext *io = nullptr;
	Ref<FFmpegDVRWindow> dvr;
	String manifest_url;
	bool primary = true;
	bool initial = true;
	CharString manifest;
	bool memory_manifest = false;
	int64_t manifest_position = 0;
	double input_start = -1;
	int64_t sequence_floor = -1;
	struct TransportErrorContext {
		Mutex mutex;
		String message;
	};
	const std::shared_ptr<TransportErrorContext> error_context;

	void set_transport_error(const String &p_operation, Error p_error) {
		const int status = http.get_response_status();
		MutexLock lock(error_context->mutex);
		error_context->message = vformat("Media transport %s failed (error %d%s).", p_operation, int(p_error), status > 0 ? vformat(", HTTP %d", status) : String());
	}
	void clear_transport_error() {
		MutexLock lock(error_context->mutex);
		error_context->message = String();
	}

	static int read(void *p_self, uint8_t *p_buffer, int p_size) {
		FFmpegRemoteIO *self = static_cast<FFmpegRemoteIO *>(p_self);
		if (self->memory_manifest) {
			if (interrupt(self)) {
				return AVERROR_EXIT;
			}
			int amount = int(MIN(int64_t(p_size), self->manifest.length() - self->manifest_position));
			if (amount <= 0) {
				return AVERROR_EOF;
			}
			memcpy(p_buffer, self->manifest.get_data() + self->manifest_position, amount);
			self->manifest_position += amount;
			return amount;
		}
		int amount = self->http.read(p_buffer, p_size);
		if (amount < 0) {
			self->set_transport_error("read", ERR_CONNECTION_ERROR);
		}
		return amount > 0 ? amount : (amount == 0 ? AVERROR_EOF : AVERROR(EIO));
	}
	static int64_t seek(void *p_self, int64_t p_offset, int p_whence) {
		FFmpegRemoteIO *self = static_cast<FFmpegRemoteIO *>(p_self);
		if (self->memory_manifest) {
			if (p_whence & AVSEEK_SIZE) {
				return self->manifest.length();
			}
			p_whence &= ~AVSEEK_FORCE;
			int64_t base = p_whence == SEEK_CUR ? self->manifest_position : (p_whence == SEEK_END ? self->manifest.length() : 0);
			if ((p_whence != SEEK_SET && p_whence != SEEK_CUR && p_whence != SEEK_END) || p_offset < -base || p_offset > self->manifest.length() - base) {
				return AVERROR(EINVAL);
			}
			self->manifest_position = base + p_offset;
			return self->manifest_position;
		}
		if (p_whence & AVSEEK_SIZE) {
			return self->http.get_size() >= 0 ? self->http.get_size() : AVERROR(ENOSYS);
		}
		p_whence &= ~AVSEEK_FORCE;
		int64_t base = 0;
		if (p_whence == SEEK_CUR) {
			base = self->http.get_position();
		} else if (p_whence == SEEK_END) {
			base = self->http.get_size();
		} else if (p_whence != SEEK_SET) {
			return AVERROR(EINVAL);
		}
		if (base < 0 || p_offset < -base || p_offset > INT64_MAX - base) {
			return AVERROR(EINVAL);
		}
		const Error error = self->http.seek(base + p_offset);
		if (error != OK) {
			self->set_transport_error("seek", error);
			return AVERROR(EIO);
		}
		self->clear_transport_error();
		return base + p_offset;
	}
	static int interrupt(void *p_self) {
		FFmpegRemoteIO *self = static_cast<FFmpegRemoteIO *>(p_self);
		return (self->cancel && self->cancel->is_set()) || (self->stop && self->stop->is_set()) || (self->absolute_deadline && OS::get_singleton()->get_ticks_usec() >= self->absolute_deadline);
	}
	static int open_secondary(AVFormatContext *p_format, AVIOContext **r_io, const char *p_url, int p_flags, AVDictionary **) {
		FFmpegRemoteIO *parent = static_cast<FFmpegRemoteIO *>(p_format->opaque);
		*r_io = nullptr;
		if (!parent || (p_flags & AVIO_FLAG_WRITE)) {
			return AVERROR(EACCES);
		}
		FFmpegRemoteIO *child = memnew(FFmpegRemoteIO(parent->cancel, parent->headers, parent->stop, parent->absolute_deadline, parent->error_context));
		String url = MediaHTTP::resolve_url(parent->http.get_url(), String::utf8(p_url));
		// Secondary media may use HTTP only while its actual parent is HTTP.
		// An HTTPS manifest cannot downgrade segments, keys or nested playlists.
		child->set_allow_http(parent->http.get_url().begins_with("http://"));
		if (parent->dvr.is_valid() && (url == parent->manifest_url || url == parent->http.get_url())) {
			child->dvr = parent->dvr;
			child->manifest_url = parent->manifest_url;
			child->primary = parent->primary;
			child->initial = false;
			child->sequence_floor = parent->sequence_floor;
		}
		int error = child->open(url);
		if (error < 0) {
			memdelete(child);
			return error;
		}
		parent->clear_transport_error();
		*r_io = child->io;
		return 0;
	}
	static int close_secondary(AVFormatContext *, AVIOContext *p_io) {
		memdelete(static_cast<FFmpegRemoteIO *>(p_io->opaque));
		return 0;
	}

public:
	FFmpegRemoteIO(const SafeFlag *p_cancel, const Dictionary &p_headers, const SafeFlag *p_stop = nullptr, uint64_t p_absolute_deadline = 0, const std::shared_ptr<TransportErrorContext> &p_error_context = {}) : http(p_cancel, p_stop, p_absolute_deadline), cancel(p_cancel), stop(p_stop), absolute_deadline(p_absolute_deadline), headers(p_headers), error_context(p_error_context ? p_error_context : std::make_shared<TransportErrorContext>()) {}
	void set_allow_http(bool p_enabled) { http.set_allow_http(p_enabled); }
	bool is_http_transport() const { return http.get_url().begins_with("http://"); }
	String get_url() const { return http.get_url(); }
	String get_error_message() const {
		MutexLock lock(error_context->mutex);
		return error_context->message;
	}
	void set_dvr(const Ref<FFmpegDVRWindow> &p_dvr, bool p_primary) {
		dvr = p_dvr;
		primary = p_primary;
	}
	double get_dvr_input_start() const { return input_start; }
	int open(const String &p_url) {
		clear_transport_error();
		String scheme, host, path, fragment;
		int port;
		const bool youtube_media = p_url.parse_url(scheme, host, port, path, fragment) == OK && host.to_lower().ends_with(".googlevideo.com");
		// Match upstream yt-dlp's YouTube media chunk policy. This is a bounded
		// streamed request, never a full-file cache; all range opens remain pinned
		// and validated by MediaHTTP. Other sources retain their current behavior.
		http.set_range_chunk_size(youtube_media ? 10 * 1024 * 1024 : 0);
		const Error open_error = cancel ? http.open(p_url, headers) : ERR_INVALID_PARAMETER;
		if (open_error != OK) {
			set_transport_error("open", open_error);
			return AVERROR(EACCES);
		}
		if (dvr.is_valid()) {
			if (manifest_url.is_empty()) {
				manifest_url = p_url;
			}
			Vector<uint8_t> body;
			uint8_t chunk[8192];
			int amount = 0;
			while ((amount = http.read(chunk, sizeof(chunk))) > 0) {
				int old_size = body.size();
				if (old_size > 1024 * 1024 - amount || body.resize(old_size + amount) != OK) {
					return AVERROR(ENOMEM);
				}
				memcpy(body.ptrw() + old_size, chunk, amount);
			}
			if (amount < 0) {
				set_transport_error("manifest read", ERR_CONNECTION_ERROR);
				return AVERROR(EIO);
			}
			String text = String::utf8(reinterpret_cast<const char *>(body.ptr()), body.size());
			String prepared;
			if (!dvr->prepare(text, primary, initial, prepared, input_start, &sequence_floor)) {
				if (dvr->required) {
					return AVERROR(ENOSYS);
				}
				prepared = text;
			}
			manifest = prepared.utf8();
			memory_manifest = true;
		}
		uint8_t *buffer = static_cast<uint8_t *>(av_malloc(32768));
		if (!buffer) {
			return AVERROR(ENOMEM);
		}
		io = avio_alloc_context(buffer, 32768, 0, this, read, nullptr, seek);
		if (!io) {
			av_free(buffer);
			return AVERROR(ENOMEM);
		}
		io->seekable = memory_manifest || http.get_size() >= 0 ? AVIO_SEEKABLE_NORMAL : 0;
		return 0;
	}
	int open_format(AVFormatContext **r_format, const String &p_url, const AVInputFormat *p_input_format) {
		int error = open(p_url);
		if (error < 0) {
			return error;
		}
		*r_format = avformat_alloc_context();
		if (!*r_format) {
			return AVERROR(ENOMEM);
		}
		(*r_format)->pb = io;
		(*r_format)->flags |= AVFMT_FLAG_CUSTOM_IO;
		(*r_format)->opaque = this;
		(*r_format)->io_open = open_secondary;
		(*r_format)->io_close2 = close_secondary;
		(*r_format)->interrupt_callback = { interrupt, this };
		// FFmpeg's protocols never open a socket or local file on this path.
		(*r_format)->protocol_whitelist = av_strdup(http.get_url().begins_with("http://") ? "http,https" : "https");
		if (!(*r_format)->protocol_whitelist) {
			avformat_free_context(*r_format);
			*r_format = nullptr;
			return AVERROR(ENOMEM);
		}
		(*r_format)->probesize = 1024 * 1024;
		(*r_format)->max_streams = 32;
		(*r_format)->max_probe_packets = 256;
		(*r_format)->max_analyze_duration = 3 * AV_TIME_BASE;
		AVDictionary *options = nullptr;
		// Finite PCM also needs a fresh demuxer at declared MAP changes.
		// Its decoder separately disables real-time clock trimming/padding.
		av_dict_set(&options, "toi_pdt_timeline", "1", 0);
		if (input_start >= 0) {
			av_dict_set(&options, "live_start_index", "0", 0);
		}
		error = avformat_open_input(r_format, p_url.utf8().get_data(), p_input_format, &options);
		av_dict_free(&options);
		return error;
	}
	~FFmpegRemoteIO() {
		if (io) {
			av_freep(&io->buffer);
			avio_context_free(&io);
		}
	}
};
