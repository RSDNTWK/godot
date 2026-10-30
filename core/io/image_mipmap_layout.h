/**************************************************************************/
/*  image_mipmap_layout.h                                                 */
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

#include "core/error/error_list.h"

#include <cstdint>

namespace ImageMipmapLayout {
struct Level {
	int64_t source_offset = 0;
	int64_t destination_offset = 0;
	int source_width = 0;
	int source_height = 0;
};
struct Layout {
	Level levels[32];
	int count = 0;
	int64_t base_bytes = 0;
	int64_t total_bytes = 0;
};
inline Error calculate(int p_width, int p_height, int p_pixel_bytes, Layout &r_layout) {
	r_layout = {};
	if (p_width <= 0 || p_height <= 0 || p_pixel_bytes <= 0) {
		return ERR_INVALID_PARAMETER;
	}
	const uint64_t pixels = uint64_t(p_width) * p_height;
	if (pixels > uint64_t(INT64_MAX) / uint64_t(p_pixel_bytes)) {
		return ERR_OUT_OF_MEMORY;
	}
	r_layout.base_bytes = int64_t(pixels * p_pixel_bytes);
	r_layout.total_bytes = r_layout.base_bytes;
	int width = p_width;
	int height = p_height;
	int64_t source_offset = 0;
	while (width > 1 || height > 1) {
		if (r_layout.count >= 32) {
			return ERR_INVALID_PARAMETER;
		}
		Level &level = r_layout.levels[r_layout.count++];
		level.source_offset = source_offset;
		level.destination_offset = r_layout.total_bytes;
		level.source_width = width;
		level.source_height = height;
		width = width > 1 ? width / 2 : 1;
		height = height > 1 ? height / 2 : 1;
		const uint64_t next_bytes = uint64_t(width) * height * p_pixel_bytes;
		if (next_bytes > uint64_t(INT64_MAX - r_layout.total_bytes)) {
			return ERR_OUT_OF_MEMORY;
		}
		source_offset = r_layout.total_bytes;
		r_layout.total_bytes += int64_t(next_bytes);
	}
	return OK;
}
} // namespace ImageMipmapLayout
