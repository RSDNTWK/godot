/**************************************************************************/
/*  theora_frame_helpers.h                                               */
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

#include "core/io/image_loader_safety.h"
#include "core/math/rect2i.h"

#include <thirdparty/misc/yuv2rgb.h>

#include <theora/theoradec.h>

namespace TheoraFrame {
inline Error prepare_rgba(const Vector2i &p_size, Vector<uint8_t> &r_data) {
	int64_t bytes = 0;
	Error error = ImageLoaderSafety::image_bytes(p_size.x, p_size.y, 4, bytes);
	if (error != OK) {
		return error;
	}
	// Always allocate a fresh buffer: a previous Image may still share r_data.
	Vector<uint8_t> data;
	error = data.resize(bytes);
	if (error != OK) {
		return error;
	}
	data.fill(0);
	r_data = std::move(data);
	return OK;
}

inline Error convert(const th_ycbcr_buffer p_planes, const Rect2i &p_region, th_pixel_fmt p_format, Vector<uint8_t> &r_data) {
	int shift_x = 0;
	int shift_y = 0;
	switch (p_format) {
		case TH_PF_420:
			shift_y = 1;
			[[fallthrough]];
		case TH_PF_422:
			shift_x = 1;
			break;
		case TH_PF_444:
			break;
		default:
			return ERR_FILE_CORRUPT;
	}
	int64_t bytes = 0;
	Error error = ImageLoaderSafety::image_bytes(p_region.size.x, p_region.size.y, 4, bytes);
	if (error != OK || p_region.position.x < 0 || p_region.position.y < 0) {
		return ERR_FILE_CORRUPT;
	}
	const int64_t last_x = int64_t(p_region.position.x) + p_region.size.x - 1;
	const int64_t last_y = int64_t(p_region.position.y) + p_region.size.y - 1;
	for (int plane = 0; plane < 3; plane++) {
		const th_img_plane &image = p_planes[plane];
		const int sx = plane == 0 ? 0 : shift_x;
		const int sy = plane == 0 ? 0 : shift_y;
		if (image.data == nullptr || image.width <= 0 || image.height <= 0 || image.stride < image.width ||
				(last_x >> sx) >= image.width || (last_y >> sy) >= image.height) {
			return ERR_FILE_CORRUPT;
		}
		// Pointer offsets must also fit the address space of 32-bit builds.
		if (uint64_t(image.height - 1) * uint64_t(image.stride) + uint64_t(image.width) > uint64_t(PTRDIFF_MAX)) {
			return ERR_FILE_CORRUPT;
		}
	}
	Vector<uint8_t> data;
	error = data.resize(bytes);
	if (error != OK) {
		return error;
	}
	yuv_to_rgb8888_cropped(data.ptrw(), p_planes[0].data, p_planes[1].data, p_planes[2].data,
			p_region.size.x, p_region.size.y, p_planes[0].stride, p_planes[1].stride, p_planes[2].stride,
			p_region.size.x * 4, shift_x, shift_y, p_region.position.x, p_region.position.y);
	r_data = std::move(data);
	return OK;
}
} // namespace TheoraFrame
