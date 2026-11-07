/**************************************************************************/
/*  ffmpeg_frame_queue.h                                              */
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

#include "core/io/image.h"
#include "core/math/math_funcs.h"

// Caller holds the player's command mutex. Immutable CPU images cross the
// worker/frontend boundary; GPU textures never enter this bounded queue.
class FFmpegFrameQueue {
	static constexpr int MAX_FRAMES = 4;
	static constexpr int64_t MAX_BYTES = 32 * 1024 * 1024;
	static constexpr int64_t MAX_FRAME_BYTES = 64 * 1024 * 1024;
	struct Frame {
		Ref<Image> image;
		double position = 0.0;
		int64_t bytes = 0;
	};
	Frame frames[MAX_FRAMES];
	int head = 0;
	int count = 0;
	int64_t bytes = 0;
	int64_t last_frame_bytes = 0;

public:
	bool has_room() const { return count < MAX_FRAMES && (count == 0 || last_frame_bytes <= MAX_BYTES - bytes); }
	int size() const { return count; }
	int64_t get_bytes() const { return bytes; }
	Ref<Image> take_first(double &r_position) {
		if (count == 0) {
			return Ref<Image>();
		}
		Frame &frame = frames[head];
		Ref<Image> selected = frame.image;
		r_position = frame.position;
		bytes -= frame.bytes;
		frame = Frame();
		head = (head + 1) % MAX_FRAMES;
		count--;
		return selected;
	}
	void clear() {
		for (Frame &frame : frames) {
			frame = Frame();
		}
		head = count = 0;
		bytes = last_frame_bytes = 0;
	}
	Error push(const Ref<Image> &p_image, double p_position) {
		if (p_image.is_null() || p_image->is_empty() || !Math::is_finite(p_position)) {
			return ERR_INVALID_DATA;
		}
		const int64_t image_bytes = p_image->get_data().size();
		if (image_bytes <= 0 || image_bytes > MAX_FRAME_BYTES) {
			return ERR_INVALID_DATA;
		}
		if (count == MAX_FRAMES || (count > 0 && image_bytes > MAX_BYTES - bytes)) {
			return ERR_BUSY;
		}
		Frame &frame = frames[(head + count) % MAX_FRAMES];
		frame.image = p_image;
		frame.position = p_position;
		frame.bytes = image_bytes;
		bytes += image_bytes;
		last_frame_bytes = image_bytes;
		count++;
		return OK;
	}
	Ref<Image> take_due(double p_clock, double &r_position, int &r_discarded, double p_frame_duration = 0.0) {
		Ref<Image> selected;
		r_discarded = 0;
		if (Math::is_finite(p_frame_duration) && p_frame_duration > 0.0) {
			// Keep frame order across small audio-clock/render-callback jitter.
			// Always selecting the newest due frame alternates repeats and drops
			// whenever a callback lands just before/after a source timestamp.
			const double early = MIN(p_frame_duration * 0.25, 0.004);
			const double late = CLAMP(p_frame_duration * 1.5, 0.025, 0.15);
			while (count > 1 && frames[head].position < p_clock - late && frames[(head + 1) % MAX_FRAMES].position <= p_clock + early) {
				Frame &frame = frames[head];
				bytes -= frame.bytes;
				frame = Frame();
				head = (head + 1) % MAX_FRAMES;
				count--;
				r_discarded++;
			}
			if (count > 0 && frames[head].position <= p_clock + early) {
				Frame &frame = frames[head];
				selected = frame.image;
				r_position = frame.position;
				bytes -= frame.bytes;
				frame = Frame();
				head = (head + 1) % MAX_FRAMES;
				count--;
			}
			return selected;
		}
		while (count > 0 && frames[head].position <= p_clock + 0.000001) {
			Frame &frame = frames[head];
			r_discarded += selected.is_valid();
			selected = frame.image;
			r_position = frame.position;
			bytes -= frame.bytes;
			frame = Frame();
			head = (head + 1) % MAX_FRAMES;
			count--;
		}
		return selected;
	}
};
