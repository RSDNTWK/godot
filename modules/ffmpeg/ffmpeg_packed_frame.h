#pragma once

#include "core/error/error_list.h"
#include "core/os/memory.h"
#include "core/templates/vector.h"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

// Own the extra SIMD tail until FFmpeg releases every reference. Godot receives
// the same storage with its logical size trimmed after conversion; queued Images
// then keep it alive through Vector's normal copy-on-write ownership.
class FFmpegPackedFrame {
	struct Backing {
		Vector<uint8_t> data;
	};
	static void release(void *p_owner, uint8_t *) {
		memdelete(static_cast<Backing *>(p_owner));
	}

public:
	static bool supports(int p_width, int p_height) {
		return p_width > 0 && p_height > 0 && !(p_width & 31) && int64_t(p_width) * p_height <= 16777216;
	}

	static Error prepare(AVFrame *p_frame, int p_width, int p_height, Vector<uint8_t> &r_data) {
		if (!p_frame || !supports(p_width, p_height)) {
			return ERR_INVALID_PARAMETER;
		}
		// Complete aligned rows plus a separately allocated tail protect SIMD
		// stores at the last row. No reliance on Vector's spare capacity.
		const int bytes = p_width * p_height * 4;
		r_data.clear();
		if (r_data.resize(int64_t(bytes) + 64) != OK) {
			return ERR_OUT_OF_MEMORY;
		}
		uint8_t *pixels = r_data.ptrw();
		Backing *owner = memnew(Backing);
		owner->data = r_data;
		AVBufferRef *buffer = av_buffer_create(pixels, size_t(bytes) + 64, release, owner, 0);
		if (!buffer) {
			memdelete(owner);
			return ERR_OUT_OF_MEMORY;
		}
		av_frame_unref(p_frame);
		p_frame->format = AV_PIX_FMT_RGBA;
		p_frame->width = p_width;
		p_frame->height = p_height;
		p_frame->buf[0] = buffer;
		p_frame->data[0] = pixels;
		p_frame->linesize[0] = p_width * 4;
		return OK;
	}

	static Error finish(AVFrame *p_frame, Vector<uint8_t> &r_data) {
		if (!p_frame || p_frame->format != AV_PIX_FMT_RGBA || !supports(p_frame->width, p_frame->height)) {
			return ERR_INVALID_PARAMETER;
		}
		const int bytes = p_frame->width * p_frame->height * 4;
		if (r_data.size() < int64_t(bytes) + 64 || p_frame->data[0] != r_data.ptr() || p_frame->linesize[0] != p_frame->width * 4) {
			return ERR_INVALID_PARAMETER;
		}
		av_frame_unref(p_frame);
		// With normal synchronous sws_scale_frame() completion this is unique
		// storage and a size-only trim. If a foreign AVFrame retained it, CoW
		// deliberately copies instead of exposing mutable shared storage.
		return r_data.resize(bytes);
	}
};
