/**************************************************************************/
/*  ffmpeg_adaptive_quality.h                                              */
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

// Worker-owned pressure accumulator. Only downshifts; a playback cannot oscillate
// between rungs. Seek/pause/opening time never counts as performance evidence.
class FFmpegAdaptiveQuality {
	uint64_t opened_usec = 0;
	uint64_t last_usec = 0;
	uint64_t next_attempt_usec = 0;
	double pressure_usec = 0;
	bool disabled = false;
	uint64_t rate_start_usec = 0;
	uint64_t rate_frames = 0;
	uint64_t rate_frontend = 0;
	double rate_clock = 0;
	double rate_expected_clock = 0;
	bool low_cadence = false;

public:
	static int lower_height(int p_height) {
		const int heights[] = { 720, 480, 360, 240, 144 };
		for (int height : heights) {
			if (height < p_height) {
				return height;
			}
		}
		return 0;
	}
	void opened(uint64_t p_now) {
		opened_usec = last_usec = p_now;
		pressure_usec = 0;
		rate_start_usec = 0;
		low_cadence = false;
	}
	void disable() { disabled = true; }
	int observe(uint64_t p_now, int p_height, bool p_pressure, bool p_suspended, uint64_t p_frames = 0, double p_clock = 0.0, double p_fps = 0.0, uint64_t p_frontend = 0, double p_expected_clock = -1.0) {
		if (disabled || p_now < last_usec) {
			return 0;
		}
		const double elapsed = MIN(double(p_now - last_usec), 250000.0);
		last_usec = p_now;
		if (p_suspended) {
			opened(p_now);
			return 0;
		}
		if (p_now - opened_usec < 5000000) {
			return 0;
		}
		if (p_fps > 0 && Math::is_finite(p_fps) && Math::is_finite(p_clock)) {
			if (rate_start_usec == 0 || p_frames < rate_frames || p_frontend < rate_frontend || p_clock < rate_clock) {
				rate_start_usec = p_now;
				rate_frames = p_frames;
				rate_frontend = p_frontend;
				rate_clock = p_clock;
				rate_expected_clock = p_expected_clock;
				low_cadence = false;
			} else if (p_now - rate_start_usec >= 2000000) {
				// A deliberately capped main loop cannot present every source
				// frame. Do not blame the decoder for the application's FPS cap.
				const double expected = MIN((p_clock - rate_clock) * p_fps, double(p_frontend - rate_frontend));
				low_cadence = expected >= p_fps && double(p_frames - rate_frames) < expected * 0.8;
				// Live A/V backpressure can keep both tracks together by slowing
				// the entire source clock. Compare with requested media time too;
				// the frontend's delta already includes user speed/time scaling.
				const double requested = p_expected_clock - rate_expected_clock;
				if (p_expected_clock >= 0.0 && rate_expected_clock >= 0.0 && requested >= 1.0) {
					low_cadence = low_cadence || p_clock - rate_clock < requested * 0.8;
				}
				rate_start_usec = p_now;
				rate_frames = p_frames;
				rate_frontend = p_frontend;
				rate_clock = p_clock;
				rate_expected_clock = p_expected_clock;
			}
		}
		p_pressure = p_pressure || low_cadence;
		pressure_usec = CLAMP(pressure_usec + (p_pressure ? elapsed : -2.0 * elapsed), 0.0, 3000000.0);
		if (pressure_usec < 3000000.0 || p_now < next_attempt_usec) {
			return 0;
		}
		pressure_usec = 0;
		next_attempt_usec = p_now + 30000000;
		return lower_height(p_height);
	}
};
