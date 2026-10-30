/**************************************************************************/
/*  exr_test_helpers.h                                                    */
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

#include "core/io/file_access_memory.h"

#include "modules/tinyexr/image_loader_tinyexr.h"

#include <cstring>

namespace TestEXRFixture {
inline void u32(Vector<uint8_t> &p_bytes, uint32_t p_value) {
	for (int i = 0; i < 4; i++) {
		p_bytes.push_back(uint8_t(p_value >> (8 * i)));
	}
}
inline void u64(Vector<uint8_t> &p_bytes, uint64_t p_value) {
	for (int i = 0; i < 8; i++) {
		p_bytes.push_back(uint8_t(p_value >> (8 * i)));
	}
}
inline void scalar(Vector<uint8_t> &p_bytes, float p_value, bool p_half = false) {
	if (p_half) {
		const uint16_t bits = Math::make_half_float(p_value);
		p_bytes.push_back(uint8_t(bits));
		p_bytes.push_back(uint8_t(bits >> 8));
	} else {
		uint32_t bits;
		memcpy(&bits, &p_value, sizeof(bits));
		u32(p_bytes, bits);
	}
}
inline void string(Vector<uint8_t> &p_bytes, const String &p_value) {
	const CharString encoded = p_value.utf8();
	for (int i = 0; i < encoded.length(); i++) {
		p_bytes.push_back(uint8_t(encoded[i]));
	}
	p_bytes.push_back(0);
}
inline void attribute(Vector<uint8_t> &p_bytes, const String &p_name, const String &p_type, const Vector<uint8_t> &p_value) {
	string(p_bytes, p_name);
	string(p_bytes, p_type);
	u32(p_bytes, p_value.size());
	p_bytes.append_array(p_value);
}
inline float base(const String &p_name) {
	if (p_name == "R") {
		return 0.125f;
	}
	if (p_name == "G") {
		return 0.25f;
	}
	if (p_name == "B") {
		return 0.375f;
	}
	if (p_name == "A") {
		return 0.5f;
	}
	if (p_name == "Y") {
		return 0.625f;
	}
	return 0.75f;
}

// Small uncompressed EXR fixtures. Channel order is explicit, as in the
// dependency's writer. Rows contain each channel's contiguous pixel samples.
inline Vector<uint8_t> fixture(const char *p_names, bool p_half, bool p_tiled, bool p_ignored_half = false, const char *p_uint_names = "", uint32_t p_uint_value = 1, int p_tile_size = 2, int p_custom_size = -1) {
	const Vector<String> names = String(p_names).split(",");
	const Vector<String> uint_names = String(p_uint_names).split(",");
	const int width = 17;
	const int height = 19;
	Vector<uint8_t> result;
	u32(result, 20000630);
	u32(result, 2 | (p_tiled ? 0x200 : 0));
	Vector<uint8_t> channels;
	for (const String &name : names) {
		string(channels, name);
		u32(channels, uint_names.has(name) ? 0 : (p_half || (p_ignored_half && name == "ignored.H")) ? 1
																									 : 2);
		u32(channels, 0); // pLinear and three reserved bytes.
		u32(channels, 1);
		u32(channels, 1);
	}
	channels.push_back(0);
	attribute(result, "channels", "chlist", channels);
	Vector<uint8_t> compression;
	compression.push_back(0);
	attribute(result, "compression", "compression", compression);
	Vector<uint8_t> window;
	u32(window, 0);
	u32(window, 0);
	u32(window, width - 1);
	u32(window, height - 1);
	attribute(result, "dataWindow", "box2i", window);
	attribute(result, "displayWindow", "box2i", window);
	attribute(result, "lineOrder", "lineOrder", compression);
	Vector<uint8_t> one;
	scalar(one, 1.0f);
	attribute(result, "pixelAspectRatio", "float", one);
	attribute(result, "screenWindowWidth", "float", one);
	Vector<uint8_t> center;
	scalar(center, 0.0f);
	scalar(center, 0.0f);
	attribute(result, "screenWindowCenter", "v2f", center);
	if (p_tiled) {
		Vector<uint8_t> tile;
		u32(tile, p_tile_size);
		u32(tile, p_tile_size);
		tile.push_back(0); // One level, rounded down.
		attribute(result, "tiles", "tiledesc", tile);
	}
	if (p_custom_size >= 0) {
		Vector<uint8_t> custom;
		REQUIRE(custom.resize(p_custom_size) == OK);
		custom.fill(0x67);
		attribute(result, "testAttribute", "opaque", custom);
	}
	result.push_back(0);
	Vector<Vector<uint8_t>> chunks;
	const int step_y = p_tiled ? p_tile_size : 1;
	const int step_x = p_tiled ? p_tile_size : width;
	for (int by = 0; by < height; by += step_y) {
		for (int bx = 0; bx < width; bx += step_x) {
			const int tile_width = MIN(step_x, width - bx);
			const int tile_height = MIN(step_y, height - by);
			Vector<uint8_t> pixels;
			for (int y = 0; y < tile_height; y++) {
				for (const String &name : names) {
					for (int x = 0; x < tile_width; x++) {
						const float value = base(name) + float(bx + x + width * (by + y)) / 64.0f;
						if (uint_names.has(name)) {
							u32(pixels, p_uint_value);
						} else {
							scalar(pixels, value, p_half || (p_ignored_half && name == "ignored.H"));
						}
					}
				}
			}
			Vector<uint8_t> chunk;
			if (p_tiled) {
				u32(chunk, bx / p_tile_size);
				u32(chunk, by / p_tile_size);
				u32(chunk, 0);
				u32(chunk, 0);
			} else {
				u32(chunk, by);
			}
			u32(chunk, pixels.size());
			chunk.append_array(pixels);
			chunks.push_back(chunk);
		}
	}
	uint64_t offset = result.size() + 8 * chunks.size();
	for (const Vector<uint8_t> &chunk : chunks) {
		u64(result, offset);
		offset += chunk.size();
	}
	for (const Vector<uint8_t> &chunk : chunks) {
		result.append_array(chunk);
	}
	return result;
}
inline Error load(const Vector<uint8_t> &p_bytes, Ref<Image> p_image, BitField<ImageFormatLoader::LoaderFlags> p_flags = 0) {
	Ref<FileAccessMemory> file = memnew(FileAccessMemory);
	const Error error = file->open_custom(p_bytes.ptr(), p_bytes.size());
	if (error != OK) {
		return error;
	}
	return ImageLoaderTinyEXR().load_image(p_image, file, p_flags, 1.0f);
}

} // namespace TestEXRFixture
