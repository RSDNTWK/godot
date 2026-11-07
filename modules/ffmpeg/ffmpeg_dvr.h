/**************************************************************************/
/*  ffmpeg_dvr.h                                              */
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

#include "core/object/ref_counted.h"
#include "core/os/mutex.h"

// One opening generation shares a UTC target between its two network inputs.
// Manifest reloads publish the actual advertised window, never a predicted one.
class FFmpegDVRWindow : public RefCounted {
	mutable Mutex mutex;
	bool available = false;
	double start = 0;
	double end = 0;
	double live = 0;
	double selected = -1;

public:
	double requested_utc = -1; // Negative means the safe live edge.
	double requested_position = -1; // First opening, before a UTC anchor exists.
	bool required = false;
	bool prepare(const String &p_body, bool p_primary, bool p_initial, String &r_body, double &r_input_start, int64_t *r_sequence_floor = nullptr);
	Dictionary snapshot() const;
};

// Protected by the asynchronous player's command mutex.
struct FFmpegDVRTimeline {
	bool enabled = false;
	bool go_live = true;
	double anchor = -1;
	double offset = 0;
	Ref<FFmpegDVRWindow> window;
	Dictionary describe(double p_position) const {
		Dictionary result;
		result["available"] = false;
		if (enabled && window.is_valid() && anchor >= 0) {
			Dictionary state = window->snapshot();
			result["available"] = state["available"];
			result["start"] = MAX(0.0, double(state["start_utc"]) - anchor);
			result["end"] = double(state["end_utc"]) - anchor;
			result["live_position"] = double(state["live_utc"]) - anchor;
			result["position"] = p_position;
			result["opened_position"] = offset;
			result["timeline_origin_utc"] = anchor;
			result["start_utc"] = state["start_utc"];
			result["end_utc"] = state["end_utc"];
			result["position_utc"] = anchor + p_position;
		}
		return result;
	}
	void accept(const Ref<FFmpegDVRWindow> &p_window) {
		window = p_window;
		offset = 0;
		if (window.is_valid() && bool(window->snapshot().get("available", false))) {
			Dictionary state = window->snapshot();
			if (anchor < 0) {
				anchor = state["start_utc"];
			}
			offset = double(state["selected_utc"]) - anchor;
		}
	}
};
