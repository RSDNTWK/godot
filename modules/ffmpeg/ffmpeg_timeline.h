/**************************************************************************/
/*  ffmpeg_timeline.h                                                   */
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

#ifndef FFMPEG_TIMELINE_H
#define FFMPEG_TIMELINE_H

#include "core/math/math_funcs.h"

extern "C" {
#include <libavformat/avformat.h>
}

inline double ffmpeg_timeline_origin(const AVFormatContext *p_format) {
	if (p_format->start_time != AV_NOPTS_VALUE) {
		return double(p_format->start_time) / AV_TIME_BASE;
	}
	bool found = false;
	double origin = 0.0;
	for (unsigned int i = 0; i < p_format->nb_streams; i++) {
		const AVStream *stream = p_format->streams[i];
		if (stream->start_time == AV_NOPTS_VALUE) {
			continue;
		}
		const double start = stream->start_time * av_q2d(stream->time_base);
		if (!found || start < origin) {
			origin = start;
			found = true;
		}
	}
	return origin;
}

inline bool ffmpeg_timestamp_for_position(double p_position, double p_origin,
		AVRational p_time_base,
		int64_t &r_timestamp) {
	if (p_time_base.num <= 0 || p_time_base.den <= 0 ||
			!Math::is_finite(p_position) || !Math::is_finite(p_origin)) {
		return false;
	}
	const double ticks = (p_origin + p_position) / av_q2d(p_time_base);
	if (!Math::is_finite(ticks) || ticks <= double(INT64_MIN) ||
			ticks >= double(INT64_MAX)) {
		return false;
	}
	r_timestamp = int64_t(Math::floor(ticks));
	return true;
}

#endif
