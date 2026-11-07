/**************************************************************************/
/*  audio_sample_clock.h                                              */
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

#include "core/math/math_funcs.h"

// Interpolate the audible position between device mix callbacks. Consumed
// source samples are the upper bound, so underruns cannot invent media time.
class AudioSampleClock {
	double origin = 0.0;
	double position = 0.0;

public:
	double update_scheduled(double p_source_end, uint64_t p_scheduled_end_frame, uint64_t p_delivered_frames, double p_since_mix, double p_output_latency, double p_device_rate, double p_source_speed) {
		if (p_device_rate <= 0.0 || p_source_speed <= 0.0) {
			return position;
		}
		const double audible_device_frame = double(p_delivered_frames) + (p_since_mix - p_output_latency) * p_device_rate;
		const double source_remaining = MAX(0.0, double(p_scheduled_end_frame) - audible_device_frame) * p_source_speed / p_device_rate;
		return update(p_source_end, 0.0, source_remaining);
	}
	void reset(double p_origin) {
		origin = position = p_origin;
	}
	double update(double p_mixed_position, double p_since_mix, double p_latency) {
		if (!Math::is_finite(p_mixed_position) || !Math::is_finite(p_since_mix) || !Math::is_finite(p_latency) || p_mixed_position < origin) {
			return position;
		}
		const double next = CLAMP(p_mixed_position + MAX(p_since_mix, 0.0) - MAX(p_latency, 0.0), origin, p_mixed_position);
		position = MIN(p_mixed_position, MAX(position, next));
		return position;
	}
};
