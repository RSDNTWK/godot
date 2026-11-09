/**************************************************************************/
/*  ffmpeg_frame_scaler.h                                                 */
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

#include <cstdint>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

// Owned by the decode worker. Two slice workers bound conversion concurrency
// without changing the existing bilinear chroma interpolation or sample clock.
class FFmpegFrameScaler {
	SwsContext *context = nullptr;
	int width = 0;
	int height = 0;
	int format = -1;

public:
	FFmpegFrameScaler() = default;
	FFmpegFrameScaler(const FFmpegFrameScaler &) = delete;
	FFmpegFrameScaler &operator=(const FFmpegFrameScaler &) = delete;

	void reset() {
		sws_freeContext(context);
		context = nullptr;
		width = height = 0;
		format = -1;
	}

	int get_threads() const { return context ? context->threads : 0; }

	int convert(const AVFrame *p_source, AVFrame *p_destination) {
		if (!p_source || !p_destination || p_source->width <= 0 || p_source->height <= 0 ||
				int64_t(p_source->width) * p_source->height > 16777216 ||
				p_source->format < 0 || !p_source->data[0] || !p_destination->data[0] ||
				p_destination->width != p_source->width || p_destination->height != p_source->height ||
				p_destination->format != AV_PIX_FMT_RGBA) {
			return AVERROR(EINVAL);
		}
		if (!context || width != p_source->width || height != p_source->height || format != p_source->format) {
			reset();
			SwsContext *candidate = sws_alloc_context();
			if (!candidate) {
				return AVERROR(ENOMEM);
			}
			candidate->src_w = candidate->dst_w = p_source->width;
			candidate->src_h = candidate->dst_h = p_source->height;
			candidate->src_format = AVPixelFormat(p_source->format);
			candidate->dst_format = AV_PIX_FMT_RGBA;
			candidate->flags = SWS_BILINEAR;
			candidate->threads = 2;
			const int result = sws_init_context(candidate, nullptr, nullptr);
			if (result < 0) {
				sws_freeContext(candidate);
				return result;
			}
			context = candidate;
			width = p_source->width;
			height = p_source->height;
			format = p_source->format;
		}
		// The frame API receives the entire source before running independent
		// output slices. Unlike sws_scale(), its success result is zero.
		return sws_scale_frame(context, p_destination, p_source);
	}

	~FFmpegFrameScaler() { reset(); }
};
