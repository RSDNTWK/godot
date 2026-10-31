/**************************************************************************/
/*  image_loader_safety.h                                                         */
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

#include "core/io/file_access.h"
#include "core/io/image.h"

namespace ImageLoaderSafety {
inline Error image_bytes(int64_t p_width, int64_t p_height, uint64_t p_pixel_bytes, int64_t &r_bytes) {
	if (p_width <= 0 || p_height <= 0 || p_width > Image::MAX_WIDTH || p_height > Image::MAX_HEIGHT || p_pixel_bytes == 0) {
		return ERR_FILE_CORRUPT;
	}
	const uint64_t pixels = uint64_t(p_width) * uint64_t(p_height);
	if (pixels > Image::MAX_PIXELS || pixels > uint64_t(INT64_MAX) / p_pixel_bytes || pixels > SIZE_MAX / p_pixel_bytes) {
		return ERR_FILE_CORRUPT;
	}
	r_bytes = int64_t(pixels * p_pixel_bytes);
	return OK;
}

inline Error read_exact(const Ref<FileAccess> &p_file, uint8_t *p_destination, uint64_t p_bytes) {
	if (p_bytes == 0) {
		return OK;
	}
	const uint64_t count = p_file->get_buffer(p_destination, p_bytes);
	const Error error = p_file->get_error();
	if (error != OK && error != ERR_FILE_EOF) {
		return error;
	}
	return count == p_bytes ? OK : ERR_FILE_CORRUPT;
}
} // namespace ImageLoaderSafety
