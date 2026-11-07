/**************************************************************************/
/*  ffmpeg_dvr.cpp                                              */
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

#include "ffmpeg_dvr.h"

#include "core/math/math_funcs.h"

extern "C" {
#include <libavutil/parseutils.h>
}

namespace {
struct Segment {
	int line = 0;
	double utc = 0;
	double duration = 0;
};

bool parse_window(const String &p_body, Vector<String> &r_lines, Vector<Segment> &r_segments, int64_t &r_sequence, double &r_target) {
	if (p_body.utf8().length() > 1024 * 1024 || p_body.count("\n") > 65536 || !p_body.begins_with("#EXTM3U")) {
		return false;
	}
	r_lines = p_body.split("\n");
	double utc = -1;
	double duration = -1;
	double total = 0;
	int segment_line = -1;
	bool sequence_seen = false;
	for (int i = 0; i < r_lines.size(); i++) {
		String line = r_lines[i].strip_edges();
		if (line.begins_with("#EXT-X-MEDIA-SEQUENCE:")) {
			String number = line.get_slice(":", 1);
			if (sequence_seen || number.length() > 19 || !number.is_valid_int() || number.to_int() < 0) {
				return false;
			}
			r_sequence = number.to_int();
			sequence_seen = true;
		} else if (line.begins_with("#EXT-X-TARGETDURATION:")) {
			String number = line.get_slice(":", 1);
			if (!number.is_valid_int() || number.length() > 2) {
				return false;
			}
			r_target = number.to_float();
		} else if (line.begins_with("#EXT-X-PROGRAM-DATE-TIME:")) {
			String date = line.substr(25);
			int64_t usec = 0;
			if (date.length() < 20 || !date.contains("T") || (!date.ends_with("Z") && !date.substr(19).contains("+") && !date.substr(19).contains("-")) || av_parse_time(&usec, date.utf8().get_data(), 0) < 0) {
				return false;
			}
			double next = double(usec) / 1000000.0;
			if (utc >= 0 && Math::abs(next - utc) > 0.25) {
				return false;
			}
			utc = next;
		} else if (line.begins_with("#EXTINF:")) {
			String number = line.substr(8).get_slice(",", 0);
			if (duration >= 0 || !number.is_valid_float()) {
				return false;
			}
			duration = number.to_float();
			segment_line = i;
			if (!Math::is_finite(duration) || duration <= 0 || duration > 30) {
				return false;
			}
		} else if (line == "#EXT-X-DISCONTINUITY" || line == "#EXT-X-ENDLIST" || line.begins_with("#EXT-X-PLAYLIST-TYPE:") || line.begins_with("#EXT-X-BYTERANGE:") || line.begins_with("#EXT-X-STREAM-INF:") || line.begins_with("#EXT-X-PART:") || (line.begins_with("#EXT-X-KEY:") && line != "#EXT-X-KEY:METHOD=NONE") || (line.begins_with("#EXT-X-MAP:") && !r_segments.is_empty())) {
			// Do not invent a timeline across timestamp epochs, encryption,
			// partial segments or implicit byte ranges. Ordinary live still works.
			return false;
		} else if (!line.is_empty() && !line.begins_with("#")) {
			if (utc < 0 || duration <= 0 || r_segments.size() >= 16384) {
				return false;
			}
			r_segments.push_back({ segment_line, utc, duration });
			utc += duration;
			total += duration;
			duration = -1;
			if (total > 48 * 3600) {
				return false;
			}
		}
	}
	return sequence_seen && !r_segments.is_empty() && duration < 0 && Math::is_finite(r_target) && r_target > 0 && r_target <= 30 && r_sequence <= INT64_MAX - r_segments.size();
}
} // namespace

bool FFmpegDVRWindow::prepare(const String &p_body, bool p_primary, bool p_initial, String &r_body, double &r_input_start, int64_t *r_sequence_floor) {
	r_body = p_body;
	Vector<String> lines;
	Vector<Segment> segments;
	int64_t sequence = 0;
	double target = 0;
	if (!parse_window(p_body, lines, segments, sequence, target)) {
		if (p_primary) {
			MutexLock lock(mutex);
			available = false;
		}
		return false;
	}
	const double first = segments[0].utc;
	const double last = segments[segments.size() - 1].utc + segments[segments.size() - 1].duration;
	double selected_utc;
	{
		MutexLock lock(mutex);
		if (p_primary && (p_initial || last >= end)) {
			// Two segments of headroom keep an oldest-window seek viable while
			// the second input opens and the sliding playlist advances.
			start = MIN(first + 2 * target, last);
			end = last;
			live = MAX(start, last - 3 * target);
			available = live > start;
			if (p_initial) {
				const double requested = requested_utc < 0 ? (requested_position < 0 ? live : start + requested_position) : requested_utc;
				selected = CLAMP(requested, start, live);
			}
		}
		if (!available || (p_initial && (selected < first || selected >= last))) {
			return false;
		}
		selected_utc = selected;
	}
	if (!p_initial && (!r_sequence_floor || *r_sequence_floor < 0)) {
		return true; // Reload the complete rolling window without changing target.
	}
	int chosen = 0;
	while (p_initial && chosen + 1 < segments.size() && segments[chosen + 1].utc <= selected_utc) {
		chosen++;
	}
	// A preceding independent segment supplies decoder reference/preroll data.
	if (p_initial && chosen > 0 && selected_utc - segments[chosen - 1].utc < 25) {
		chosen--;
	}
	if (r_sequence_floor) {
		if (p_initial) {
			*r_sequence_floor = sequence + chosen;
		} else {
			// A rolling reload must not move FFmpeg's playlist sequence back
			// to the history that was omitted on this generation's first open.
			if (*r_sequence_floor >= sequence + segments.size()) {
				return false;
			}
			chosen = int(MAX(*r_sequence_floor - sequence, int64_t(0)));
		}
	}
	r_input_start = segments[chosen].utc;
	r_body = String();
	for (int i = 0; i < lines.size(); i++) {
		const String line = lines[i].strip_edges();
		if (line.begins_with("#EXT-X-PROGRAM-DATE-TIME:") || line.begins_with("#EXT-X-START:")) {
			continue;
		}
		if (line.begins_with("#EXT-X-MEDIA-SEQUENCE:")) {
			r_body += "#EXT-X-MEDIA-SEQUENCE:" + itos(sequence + chosen) + "\n";
		} else if (i < segments[0].line || i >= segments[chosen].line) {
			r_body += lines[i] + "\n";
		}
	}
	return true;
}

Dictionary FFmpegDVRWindow::snapshot() const {
	MutexLock lock(mutex);
	Dictionary result;
	result["available"] = available;
	result["start_utc"] = start;
	result["end_utc"] = end;
	result["live_utc"] = live;
	result["selected_utc"] = selected;
	return result;
}
