/**************************************************************************/
/*  texture_loader_webp.cpp                                               */
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

#include "texture_loader_webp.h"

#include "webp_common.h"

#include "core/io/file_access.h"
#include "scene/resources/animated_texture.h"
#include "scene/resources/image_texture.h"

#include <webp/demux.h>
#include <cstring>
#include <limits>

static Ref<Resource> _load_static_webp_texture(const uint8_t *p_data, int p_size, Error *r_error) {
	Ref<Image> image;
	image.instantiate();

	Error err = WebPCommon::webp_load_image_from_buffer(image.ptr(), p_data, p_size);
	if (err != OK) {
		if (r_error) {
			*r_error = err;
		}
		return Ref<Resource>();
	}

	Ref<ImageTexture> texture = ImageTexture::create_from_image(image);
	if (texture.is_null()) {
		if (r_error) {
			*r_error = ERR_CANT_CREATE;
		}
		return Ref<Resource>();
	}

	if (r_error) {
		*r_error = OK;
	}
	return texture;
}

static Ref<Resource> _load_animated_webp_texture(const uint8_t *p_data, size_t p_size, Error *r_error) {
	WebPData webp_data;
	webp_data.bytes = p_data;
	webp_data.size = p_size;

	WebPAnimDecoder *decoder = WebPAnimDecoderNew(&webp_data, nullptr);
	if (decoder == nullptr) {
		if (r_error) {
			*r_error = ERR_FILE_CORRUPT;
		}
		return Ref<Resource>();
	}

	WebPAnimInfo info;
	if (!WebPAnimDecoderGetInfo(decoder, &info)) {
		WebPAnimDecoderDelete(decoder);
		if (r_error) {
			*r_error = ERR_FILE_CORRUPT;
		}
		return Ref<Resource>();
	}

	const int frame_count = int(info.frame_count);

	Ref<AnimatedTexture> animated_texture;
	animated_texture.instantiate();
	animated_texture->set_frames(frame_count);
	// WebP loop_count == 0 means infinite looping; any non-zero value is finite.
	// AnimatedTexture currently supports one-shot vs looping, so finite WebP loops
	// are mapped to one-shot playback.
	animated_texture->set_one_shot(info.loop_count != 0);

	const int image_data_size = info.canvas_width * info.canvas_height * 4;
	int prev_timestamp_msec = 0;

	for (int frame_idx = 0; frame_idx < frame_count; frame_idx++) {
		uint8_t *frame_rgba = nullptr;
		int timestamp_msec = 0;
		if (!WebPAnimDecoderGetNext(decoder, &frame_rgba, &timestamp_msec) || frame_rgba == nullptr) {
			WebPAnimDecoderDelete(decoder);
			if (r_error) {
				*r_error = ERR_FILE_CORRUPT;
			}
			return Ref<Resource>();
		}

		Vector<uint8_t> frame_data;
		frame_data.resize(image_data_size);
		memcpy(frame_data.ptrw(), frame_rgba, image_data_size);

		Ref<Image> frame_image = memnew(Image(info.canvas_width, info.canvas_height, false, Image::FORMAT_RGBA8, frame_data));
		Ref<ImageTexture> frame_texture = ImageTexture::create_from_image(frame_image);
		if (frame_texture.is_null()) {
			WebPAnimDecoderDelete(decoder);
			if (r_error) {
				*r_error = ERR_CANT_CREATE;
			}
			return Ref<Resource>();
		}

		animated_texture->set_frame_texture(frame_idx, frame_texture);

		int duration_msec = timestamp_msec - prev_timestamp_msec;
		if (duration_msec <= 0) {
			duration_msec = 1;
		}
		animated_texture->set_frame_duration(frame_idx, MAX(0.001f, float(duration_msec) / 1000.0f));
		prev_timestamp_msec = timestamp_msec;
	}

	WebPAnimDecoderDelete(decoder);

	if (r_error) {
		*r_error = OK;
	}
	return animated_texture;
}

Ref<Resource> ResourceFormatWebP::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	return load_texture(p_path, r_error);
}

Error ResourceFormatWebP::get_webp_frame_count(const String &p_path, uint32_t &r_frame_count) {
	r_frame_count = 0;

	Error err = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ, &err);
	ERR_FAIL_COND_V(file.is_null(), err);

	const uint64_t file_size = file->get_length();
	if (file_size == 0 || file_size > uint64_t(std::numeric_limits<int>::max())) {
		return ERR_FILE_CORRUPT;
	}

	Vector<uint8_t> data;
	data.resize(file_size);
	file->get_buffer(data.ptrw(), file_size);
	if (file->get_error() != OK) {
		return file->get_error();
	}

	const uint8_t *data_ptr = data.ptr();
	const int data_size = data.size();

	WebPData webp_data;
	webp_data.bytes = data_ptr;
	webp_data.size = data_size;

	WebPDemuxer *demux = WebPDemux(&webp_data);
	if (demux != nullptr) {
		r_frame_count = WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT);
		WebPDemuxDelete(demux);
	}

	return OK;
}

Error ResourceFormatWebP::is_animated_webp(const String &p_path, bool &r_is_animated) {
	uint32_t frame_count = 0;
	Error err = get_webp_frame_count(p_path, frame_count);
	if (err != OK) {
		r_is_animated = false;
		return err;
	}

	r_is_animated = frame_count > 1;
	return OK;
}

Ref<Resource> ResourceFormatWebP::load_texture(const String &p_path, Error *r_error) {
	if (r_error) {
		*r_error = ERR_CANT_OPEN;
	}

	uint32_t frame_count = 0;
	Error err = get_webp_frame_count(p_path, frame_count);
	if (err != OK) {
		if (r_error) {
			*r_error = err;
		}
		return Ref<Resource>();
	}

	Error file_err = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ, &file_err);
	if (file.is_null()) {
		if (r_error) {
			*r_error = file_err;
		}
		return Ref<Resource>();
	}

	Vector<uint8_t> data;
	data.resize(file->get_length());
	file->get_buffer(data.ptrw(), data.size());
	if (file->get_error() != OK) {
		if (r_error) {
			*r_error = file->get_error();
		}
		return Ref<Resource>();
	}

	if (frame_count > 1) {
		return _load_animated_webp_texture(data.ptr(), data.size(), r_error);
	}

	return _load_static_webp_texture(data.ptr(), data.size(), r_error);
}

void ResourceFormatWebP::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("webp");
}

bool ResourceFormatWebP::handles_type(const String &p_type) const {
	return ClassDB::is_parent_class(p_type, "Texture");
}

String ResourceFormatWebP::get_resource_type(const String &p_path) const {
	if (p_path.has_extension("webp")) {
		return "Texture";
	}
	return "";
}
