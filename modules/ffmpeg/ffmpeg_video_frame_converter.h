/**************************************************************************/
/*  ffmpeg_video_frame_converter.h                                        */
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
#include "core/io/image.h"

struct AVFrame;
struct SwsContext;
class ImageTexture;

// Private to the FFmpeg module; validates and owns one cached conversion context.
class FFmpegVideoFrameConverter {
	SwsContext *context = nullptr;

public:
	static Error get_rgba_size(int p_width, int p_height, int64_t &r_size);
	Error convert(const AVFrame *p_frame, Ref<Image> &r_image);
	static Error update_texture(const Ref<Image> &p_image, const Ref<ImageTexture> &p_texture);
	void clear();

	FFmpegVideoFrameConverter() = default;
	FFmpegVideoFrameConverter(const FFmpegVideoFrameConverter &) = delete;
	FFmpegVideoFrameConverter &operator=(const FFmpegVideoFrameConverter &) = delete;
	~FFmpegVideoFrameConverter();
};
