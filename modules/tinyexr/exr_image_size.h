/**************************************************************************/
/*  exr_image_size.h                                                      */
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

namespace EXRImageSize {
inline Error output_bytes(int64_t p_width, int64_t p_height, int p_channels, int p_channel_bytes, int64_t &r_bytes) {
	r_bytes = 0;
	if (p_width <= 0 || p_height <= 0 || p_width > Image::MAX_WIDTH || p_height > Image::MAX_HEIGHT ||
			p_width * p_height > Image::MAX_PIXELS || p_channels < 1 || p_channels > 4 ||
			(p_channel_bytes != 2 && p_channel_bytes != 4)) {
		return ERR_FILE_CORRUPT;
	}
	const uint64_t bytes = uint64_t(p_width) * uint64_t(p_height) * p_channels * p_channel_bytes;
	if (bytes > SIZE_MAX) {
		return ERR_OUT_OF_MEMORY;
	}
	r_bytes = int64_t(bytes);
	return OK;
}
inline Error tile_rectangle(int p_width, int p_height, int p_tile_width, int p_tile_height, int p_tile_x, int p_tile_y, int p_actual_width, int p_actual_height, int64_t &r_x, int64_t &r_y) {
	r_x = 0;
	r_y = 0;
	if (p_width <= 0 || p_height <= 0 || p_tile_width <= 0 || p_tile_height <= 0 ||
			p_tile_x < 0 || p_tile_y < 0 || p_actual_width <= 0 || p_actual_height <= 0 ||
			p_actual_width > p_tile_width || p_actual_height > p_tile_height) {
		return ERR_FILE_CORRUPT;
	}
	const int64_t x = int64_t(p_tile_x) * p_tile_width;
	const int64_t y = int64_t(p_tile_y) * p_tile_height;
	if (x >= p_width || y >= p_height || p_actual_width > p_width - x || p_actual_height > p_height - y) {
		return ERR_FILE_CORRUPT;
	}
	r_x = x;
	r_y = y;
	return OK;
}
} // namespace EXRImageSize
