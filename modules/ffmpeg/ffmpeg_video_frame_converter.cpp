/**************************************************************************/
/*  ffmpeg_video_frame_converter.cpp                                      */
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

#include "ffmpeg_video_frame_converter.h"

#include "scene/resources/image_texture.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

Error FFmpegVideoFrameConverter::get_rgba_size(int p_width, int p_height, int64_t &r_size) {
	r_size = 0;
	if (p_width <= 0 || p_height <= 0 || p_width > Image::MAX_WIDTH || p_height > Image::MAX_HEIGHT ||
			int64_t(p_width) * p_height > Image::MAX_PIXELS) {
		return ERR_INVALID_DATA;
	}
	r_size = int64_t(p_width) * p_height * 4;
	return OK;
}

Error FFmpegVideoFrameConverter::convert(const AVFrame *p_frame, Ref<Image> &r_image) {
	if (!p_frame) {
		return ERR_INVALID_PARAMETER;
	}
	int64_t size = 0;
	Error error = get_rgba_size(p_frame->width, p_frame->height, size);
	if (error != OK) {
		return error;
	}
	const AVPixelFormat format = static_cast<AVPixelFormat>(p_frame->format);
	const AVPixFmtDescriptor *descriptor = av_pix_fmt_desc_get(format);
	if (!descriptor || (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
		return ERR_INVALID_DATA;
	}
	Vector<uint8_t> data;
	error = data.resize(size);
	if (error != OK) {
		return error;
	}
	context = sws_getCachedContext(context, p_frame->width, p_frame->height, format,
			p_frame->width, p_frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
	if (!context) {
		return ERR_OUT_OF_MEMORY;
	}
	// sws_scale reads four destination pointers and strides even for packed RGBA.
	uint8_t *destinations[4] = { data.ptrw(), nullptr, nullptr, nullptr };
	int strides[4] = { p_frame->width * 4, 0, 0, 0 };
	const int rows = sws_scale(context, p_frame->data, p_frame->linesize, 0, p_frame->height, destinations, strides);
	if (rows != p_frame->height) {
		return ERR_INVALID_DATA;
	}
	Ref<Image> image = Image::create_from_data(p_frame->width, p_frame->height, false, Image::FORMAT_RGBA8, data);
	if (image.is_null() || image->is_empty()) {
		return ERR_OUT_OF_MEMORY;
	}
	r_image = image;
	return OK;
}

Error FFmpegVideoFrameConverter::update_texture(const Ref<Image> &p_image, const Ref<ImageTexture> &p_texture) {
	if (p_image.is_null() || p_image->is_empty() || p_texture.is_null() ||
			p_image->get_format() != Image::FORMAT_RGBA8 || p_image->has_mipmaps()) {
		return ERR_INVALID_PARAMETER;
	}
	if (p_texture->get_width() != p_image->get_width() || p_texture->get_height() != p_image->get_height() ||
			p_texture->get_format() != p_image->get_format()) {
		p_texture->set_image(p_image);
	} else {
		p_texture->update(p_image);
	}
	return OK;
}

void FFmpegVideoFrameConverter::clear() {
	sws_freeContext(context);
	context = nullptr;
}

FFmpegVideoFrameConverter::~FFmpegVideoFrameConverter() {
	clear();
}
