/**************************************************************************/
/*  jpeg_test_helpers.h                                                        */
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
#include "core/templates/vector.h"

// Internal loader entry point: exercise its error contract without the public
// Image adapter converting every decoder error to ERR_PARSE_ERROR.
Error jpeg_turbo_load_image_from_buffer(Image *p_image, const uint8_t *p_buffer, int p_buffer_len);

namespace TestJPEG {
inline int frame_offset(const Vector<uint8_t> &p_bytes) {
	if (p_bytes.size() < 4 || p_bytes[0] != 0xff || p_bytes[1] != 0xd8) {
		return -1;
	}
	int64_t offset = 2;
	while (offset + 3 < p_bytes.size()) {
		if (p_bytes[offset++] != 0xff) {
			return -1;
		}
		while (offset < p_bytes.size() && p_bytes[offset] == 0xff) {
			offset++;
		}
		if (offset + 2 >= p_bytes.size()) {
			return -1;
		}
		const int marker = p_bytes[offset++];
		if (marker == 0xda || marker == 0xd9) {
			return -1;
		}
		const int length = (int(p_bytes[offset]) << 8) | p_bytes[offset + 1];
		if (length < 2 || length > p_bytes.size() - offset) {
			return -1;
		}
		if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
			return length >= 8 ? int(offset + 2) : -1;
		}
		offset += length;
	}
	return -1;
}

inline bool dimensions(Vector<uint8_t> &p_bytes, int p_width, int p_height) {
	const int offset = frame_offset(p_bytes);
	if (offset < 0) {
		return false;
	}
	p_bytes.write[offset + 1] = uint8_t(p_height >> 8);
	p_bytes.write[offset + 2] = uint8_t(p_height);
	p_bytes.write[offset + 3] = uint8_t(p_width >> 8);
	p_bytes.write[offset + 4] = uint8_t(p_width);
	return true;
}
} // namespace TestJPEG
