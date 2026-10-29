/**************************************************************************/
/*  test_exr_channels.h                                                        */
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
#include "tests/test_macros.h"
#include "tests/test_utils.h"

#include "modules/tinyexr/image_loader_tinyexr.h"

#include <cstring>

namespace TestEXRChannels {

struct QuietErrors {
	bool previous = CoreGlobals::print_error_enabled;
	QuietErrors() { CoreGlobals::print_error_enabled = false; }
	~QuietErrors() { CoreGlobals::print_error_enabled = previous; }
};

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
inline Vector<uint8_t> fixture(const char *p_names, bool p_half, bool p_tiled, bool p_ignored_half = false) {
	const Vector<String> names = String(p_names).split(",");
	const int width = 3;
	const int height = 3;
	Vector<uint8_t> result;
	u32(result, 20000630);
	u32(result, 2 | (p_tiled ? 0x200 : 0));
	Vector<uint8_t> channels;
	for (const String &name : names) {
		string(channels, name);
		u32(channels, (p_half || (p_ignored_half && name == "ignored.H")) ? 1 : 2);
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
		u32(tile, 2);
		u32(tile, 2);
		tile.push_back(0); // One level, rounded down.
		attribute(result, "tiles", "tiledesc", tile);
	}
	result.push_back(0);
	Vector<Vector<uint8_t>> chunks;
	const int step_y = p_tiled ? 2 : 1;
	const int step_x = p_tiled ? 2 : width;
	for (int by = 0; by < height; by += step_y) {
		for (int bx = 0; bx < width; bx += step_x) {
			const int tile_width = MIN(step_x, width - bx);
			const int tile_height = MIN(step_y, height - by);
			Vector<uint8_t> pixels;
			for (int y = 0; y < tile_height; y++) {
				for (const String &name : names) {
					for (int x = 0; x < tile_width; x++) {
						const float value = base(name) + float(bx + x + width * (by + y)) / 64.0f;
						scalar(pixels, value, p_half || (p_ignored_half && name == "ignored.H"));
					}
				}
			}
			Vector<uint8_t> chunk;
			if (p_tiled) {
				u32(chunk, bx / 2);
				u32(chunk, by / 2);
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

TEST_CASE("[F03][EXR] Supported channels preserve exact pixels in scanline and edge tiles") {
	struct Layout {
		const char *names;
		int channels;
		Color first;
	};
	const Layout layouts[] = {
		{ "R", 1, Color(0.125, 0, 0, 1) },
		{ "R,G", 2, Color(0.125, 0.25, 0, 1) },
		{ "R,G,B", 3, Color(0.125, 0.25, 0.375, 1) },
		{ "R,G,B,A", 4, Color(0.125, 0.25, 0.375, 0.5) },
		{ "Y", 3, Color(0.625, 0.625, 0.625, 1) },
		{ "Y,A", 4, Color(0.625, 0.625, 0.625, 0.5) },
		{ "R,G,B,Y", 3, Color(0.625, 0.625, 0.625, 1) },
		{ "Y,R,G,B", 3, Color(0.125, 0.25, 0.375, 1) },
		{ "R,G,B,beauty.A", 3, Color(0.125, 0.25, 0.375, 1) },
	};
	const Image::Format floats[] = { Image::FORMAT_RF, Image::FORMAT_RGF, Image::FORMAT_RGBF, Image::FORMAT_RGBAF };
	const Image::Format halves[] = { Image::FORMAT_RH, Image::FORMAT_RGH, Image::FORMAT_RGBH, Image::FORMAT_RGBAH };
	for (bool half : { false, true }) {
		for (bool tiled : { false, true }) {
			for (const Layout &layout : layouts) {
				const Vector<uint8_t> encoded = fixture(layout.names, half, tiled);
				Ref<Image> image = memnew(Image);
				REQUIRE(load(encoded, image) == OK);
				CHECK(image->get_width() == 3);
				CHECK(image->get_height() == 3);
				CHECK(image->get_format() == (half ? halves[layout.channels - 1] : floats[layout.channels - 1]));
				CHECK(image->get_data().size() == 9 * layout.channels * (half ? 2 : 4));
				for (int y = 0; y < 3; y++) {
					for (int x = 0; x < 3; x++) {
						Color expected = layout.first;
						const float delta = float(x + 3 * y) / 64.0f;
						expected.r += delta;
						if (layout.channels >= 2) {
							expected.g += delta;
						}
						if (layout.channels >= 3) {
							expected.b += delta;
						}
						if (layout.channels == 4) {
							expected.a += delta;
						}
						CHECK(image->get_pixel(x, y) == expected);
					}
				}
				Ref<Image> public_buffer = memnew(Image);
				REQUIRE(public_buffer->load_exr_from_buffer(encoded) == OK);
				CHECK(public_buffer->get_data() == image->get_data());
			}
		}
	}
}

TEST_CASE("[F03][EXR] Incomplete mapped colors reject without changing the destination") {
	const char *invalid[] = { "A", "A,G", "A,B", "A,beauty.R,beauty.G,beauty.B", "R,A", "R,G,A", "R,B,A", "beauty.R,beauty.G,beauty.B,beauty.A", "G", "B", "R,B" };
	QuietErrors quiet;
	for (bool half : { false, true }) {
		for (bool tiled : { false, true }) {
			for (const char *names : invalid) {
				const Vector<uint8_t> encoded = fixture(names, half, tiled);
				Ref<Image> destination = memnew(Image(2, 3, false, Image::FORMAT_RGBA8));
				destination->fill(Color(0.25, 0.5, 0.75, 1.0));
				const Vector<uint8_t> original = destination->get_data();
				for (int repeat = 0; repeat < 2; repeat++) {
					CHECK(load(encoded, destination) == ERR_FILE_CORRUPT);
					CHECK(destination->get_width() == 2);
					CHECK(destination->get_height() == 3);
					CHECK(destination->get_format() == Image::FORMAT_RGBA8);
					CHECK(destination->get_data() == original);
					CHECK(destination->load_exr_from_buffer(encoded) == ERR_PARSE_ERROR);
					CHECK(destination->get_data() == original);
				}
			}
		}
	}
}

TEST_CASE("[F03][EXR] Ignored HALF channels preserve output precision") {
	for (bool tiled : { false, true }) {
		const Vector<uint8_t> encoded = fixture("R,G,B,ignored.H", false, tiled, true);
		Ref<Image> image = memnew(Image);
		REQUIRE(load(encoded, image) == OK);
		CHECK(image->get_format() == Image::FORMAT_RGBH);
		CHECK(image->get_data().size() == 3 * 3 * 3 * 2);
		CHECK(image->get_pixel(2, 2) == Color(0.25, 0.375, 0.5, 1));
	}
}

TEST_CASE("[F03][EXR] Truncated loads clean up and permit subsequent valid loads") {
	for (bool tiled : { false, true }) {
		const Vector<uint8_t> encoded = fixture("R,G,B,A", false, tiled);
		const int64_t lengths[] = { 1, 4, 8, 32, 100, encoded.size() - 1 };
		QuietErrors quiet;
		for (int64_t length : lengths) {
			Vector<uint8_t> truncated = encoded;
			truncated.resize(length);
			for (int repeat = 0; repeat < 3; repeat++) {
				Ref<Image> image = memnew(Image(2, 3, false, Image::FORMAT_RGBA8));
				const Vector<uint8_t> original = image->get_data();
				CHECK(load(truncated, image) == ERR_FILE_CORRUPT);
				CHECK(image->get_data() == original);
				REQUIRE(load(encoded, image) == OK);
				CHECK(image->get_width() == 3);
				CHECK(image->get_data().size() == 3 * 3 * 4 * 4);
			}
		}
	}
}

TEST_CASE("[F03][EXR] Legacy exported asset and force-linear loading remain compatible") {
	const String path = TestUtils::get_data_path("images/f03_legacy_rgba.exr");
	const Vector<uint8_t> encoded = FileAccess::get_file_as_bytes(path);
	REQUIRE_FALSE(encoded.is_empty());
	Ref<Image> memory_image = memnew(Image);
	REQUIRE(memory_image->load_exr_from_buffer(encoded) == OK);
	Ref<Image> file_image = memnew(Image);
	REQUIRE(file_image->load(path) == OK);
	CHECK(file_image->get_data() == memory_image->get_data());
	// The original upstream fixture omits required pixelAspectRatio metadata.
	// Both the official parser and the preserved B01 build reject it.
	{
		const Vector<uint8_t> invalid = FileAccess::get_file_as_bytes(TestUtils::get_data_path("images/icon.exr"));
		REQUIRE_FALSE(invalid.is_empty());
		Ref<Image> rejected = memnew(Image);
		QuietErrors quiet;
		CHECK(load(invalid, rejected) == ERR_FILE_CORRUPT);
		CHECK(rejected->is_empty());
	}
	for (bool half : { false, true }) {
		for (bool tiled : { false, true }) {
			Ref<Image> image = memnew(Image);
			REQUIRE(load(fixture("R,G,B,A", half, tiled), image, ImageFormatLoader::FLAG_FORCE_LINEAR) == OK);
			const Color expected = Color(0.125, 0.25, 0.375, 0.5).srgb_to_linear();
			const Color actual = image->get_pixel(0, 0);
			const float tolerance = half ? 0.0005f : 0.000001f;
			CHECK(Math::abs(actual.r - expected.r) < tolerance);
			CHECK(Math::abs(actual.g - expected.g) < tolerance);
			CHECK(Math::abs(actual.b - expected.b) < tolerance);
			CHECK(actual.a == expected.a);
		}
	}
}
} // namespace TestEXRChannels
