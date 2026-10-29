/**************************************************************************/
/*  ffmpeg_ogg_helpers.h                                                  */
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

#include "thirdparty/libogg/ogg/ogg.h"

#include "core/error/error_list.h"
#include "core/templates/vector.h"

#include <cstring>

// Both libogg output paths must establish the entire destination before copying.
inline Error ffmpeg_ogg_append_size(int64_t p_size, int64_t p_header_size, int64_t p_body_size, int64_t &r_size) {
	if (p_size < 0 || p_header_size < 0 || p_body_size < 0 ||
			p_header_size > INT64_MAX - p_size || p_body_size > INT64_MAX - p_size - p_header_size) {
		return ERR_INVALID_DATA;
	}
	r_size = p_size + p_header_size + p_body_size;
	return OK;
}

inline Error ffmpeg_append_ogg_page(Vector<uint8_t> &r_data, const ogg_page &p_page) {
	const int64_t old_size = r_data.size();
	int64_t new_size = 0;
	Error error = ffmpeg_ogg_append_size(old_size, p_page.header_len, p_page.body_len, new_size);
	if (error != OK || (p_page.header_len > 0 && !p_page.header) || (p_page.body_len > 0 && !p_page.body)) {
		return ERR_INVALID_DATA;
	}
	if (new_size == old_size) {
		return OK;
	}
	error = r_data.resize(new_size);
	if (error != OK) {
		return error;
	}
	uint8_t *dest = r_data.ptrw() + old_size;
	if (p_page.header_len > 0) {
		memcpy(dest, p_page.header, size_t(p_page.header_len));
	}
	if (p_page.body_len > 0) {
		memcpy(dest + p_page.header_len, p_page.body, size_t(p_page.body_len));
	}
	return OK;
}
