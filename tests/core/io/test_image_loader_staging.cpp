/**************************************************************************/
/*  test_image_loader_staging.cpp                                         */
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

#include "tests/test_macros.h"
TEST_FORCE_LINK(test_image_loader_staging)

#include "core/io/file_access_memory.h"
#include "tests/core/io/allocation_test_helpers.h"
#include "tests/test_utils.h"

#include "modules/modules_enabled.gen.h"
#ifdef MODULE_JPG_ENABLED
#include "modules/jpg/image_loader_libjpeg_turbo.h"
#endif
#ifdef MODULE_TINYEXR_ENABLED
#include "modules/tinyexr/exr_image_size.h"
#include "modules/tinyexr/image_loader_tinyexr.h"
#endif

namespace TestImageLoaderStaging {
struct QuietErrors {
	bool previous = CoreGlobals::print_error_enabled;
	QuietErrors() { CoreGlobals::print_error_enabled = false; }
	~QuietErrors() { CoreGlobals::print_error_enabled = previous; }
};
class Input : public FileAccessMemory {
public:
	uint64_t advertised = 0;
	uint64_t limit = UINT64_MAX;
	Error error = OK;
	mutable int reads = 0;
	virtual uint64_t get_length() const override { return advertised; }
	virtual uint64_t get_buffer(uint8_t *p_dst, uint64_t p_length) const override {
		reads++;
		return FileAccessMemory::get_buffer(p_dst, MIN(p_length, limit));
	}
	virtual Error get_error() const override { return reads ? error : OK; }
};
static Ref<Input> input(const Vector<uint8_t> &p_bytes) {
	Ref<Input> file = memnew(Input);
	REQUIRE(file->open_custom(p_bytes.ptr(), p_bytes.size()) == OK);
	file->advertised = p_bytes.size();
	return file;
}
static Ref<Image> sentinel() {
	Ref<Image> image = Image::create_empty(2, 2, false, Image::FORMAT_RGBA8);
	image->fill(Color(0.125, 0.25, 0.5, 1));
	return image;
}
template <typename T>
static void staging(T &p_loader, const Vector<uint8_t> &p_bytes) {
	for (int mode = 0; mode < 4; mode++) {
		Ref<Input> file = input(p_bytes);
		Ref<Image> destination = sentinel();
		const Vector<uint8_t> before = destination->get_data();
		file->error = mode == 0 || mode == 2 ? ERR_FILE_CANT_READ : OK;
		if (mode < 2) {
			file->limit = p_bytes.size() - 1;
		} else if (mode == 3) {
			file->error = ERR_FILE_EOF;
		}
		QuietErrors quiet;
		const Error expected = mode == 0 || mode == 2 ? ERR_FILE_CANT_READ : mode == 1 ? ERR_FILE_CORRUPT
																					   : OK;
		CHECK(p_loader.load_image(destination, file, 0, 1.0) == expected);
		CHECK(file->reads == 1);
		if (mode != 3) {
			CHECK(destination->get_size() == Vector2i(2, 2));
			CHECK(destination->get_data() == before);
		}
	}
	Ref<Input> file = input(p_bytes);
	Ref<Image> destination = sentinel();
	REQUIRE(p_loader.load_image(destination, file, 0, 1.0) == OK);
	CHECK_FALSE(destination->is_empty());
}
#ifdef MODULE_JPG_ENABLED
TEST_CASE("[N030][ImageStaging] JPEG short reads and hard errors propagate before decode") {
	ImageLoaderLibJPEGTurbo loader;
	staging(loader, FileAccess::get_file_as_bytes(TestUtils::get_data_path("images/icon.jpg")));
}
TEST_CASE("[N030][ImageStaging] JPEG unrepresentable source lengths reject without reading") {
	const Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(TestUtils::get_data_path("images/icon.jpg"));
	ImageLoaderLibJPEGTurbo loader;
	for (uint64_t length : { uint64_t(INT_MAX) + 1, UINT64_MAX }) {
		Ref<Input> file = input(bytes);
		file->advertised = length;
		Ref<Image> destination = sentinel();
		const Vector<uint8_t> before = destination->get_data();
		QuietErrors quiet;
		CHECK(loader.load_image(destination, file, 0, 1.0) == ERR_FILE_CORRUPT);
		CHECK(file->reads == 0);
		CHECK(destination->get_data() == before);
	}
}
#endif
#ifdef MODULE_TINYEXR_ENABLED
TEST_CASE("[N030][ImageStaging] EXR short reads and hard errors propagate before decode") {
	ImageLoaderTinyEXR loader;
	staging(loader, FileAccess::get_file_as_bytes(TestUtils::get_data_path("images/f03_legacy_rgba.exr")));
}
TEST_CASE("[N029][EXROutput] Wide byte arithmetic handles the signed int boundary") {
	int64_t size = -1;
	CHECK(EXRImageSize::output_bytes(16384, 8192, 4, 4, size) == OK);
	CHECK(size == INT64_C(2147483648));
	for (int channels = 1; channels <= 4; channels++) {
		for (int channel_bytes : { 2, 4 }) {
			CHECK(EXRImageSize::output_bytes(17, 19, channels, channel_bytes, size) == OK);
			CHECK(size == 17 * 19 * channels * channel_bytes);
		}
	}
	CHECK(EXRImageSize::output_bytes(INT64_MAX, INT64_MAX, 4, 4, size) == ERR_FILE_CORRUPT);
	CHECK(size == 0);
	CHECK(EXRImageSize::output_bytes(16385, 16384, 4, 4, size) == ERR_FILE_CORRUPT);
	CHECK(EXRImageSize::output_bytes(0, 1, 4, 4, size) == ERR_FILE_CORRUPT);
}
#endif
#ifdef B01_ALLOCATION_TESTS
template <typename T>
static void input_failure(T &p_loader, const Vector<uint8_t> &p_bytes) {
	const size_t allocation = TestAllocation::allocation_size([&]() {
		Vector<uint8_t> probe;
		REQUIRE(probe.resize(p_bytes.size()) == OK);
	});
	Ref<Input> file = input(p_bytes);
	Ref<Image> destination = sentinel();
	const Vector<uint8_t> before = destination->get_data();
	QuietErrors quiet;
	{
		TestAllocation::Failure failure(allocation);
		CHECK(p_loader.load_image(destination, file, 0, 1.0) == ERR_OUT_OF_MEMORY);
		CHECK(TestAllocation::failures() == 1);
	}
	CHECK(file->reads == 0);
	CHECK(destination->get_data() == before);
}
#ifdef MODULE_JPG_ENABLED
TEST_CASE("[N030][ImageStagingAllocation] JPEG input allocation failure preserves destination") {
	ImageLoaderLibJPEGTurbo loader;
	input_failure(loader, FileAccess::get_file_as_bytes(TestUtils::get_data_path("images/icon.jpg")));
}
#endif
#ifdef MODULE_TINYEXR_ENABLED
namespace EXRFixture {
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
	const int width = 17;
	const int height = 19;
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

} //namespace EXRFixture
TEST_CASE("[N030][ImageStagingAllocation] EXR input allocation failure preserves destination") {
	ImageLoaderTinyEXR loader;
	input_failure(loader, FileAccess::get_file_as_bytes(TestUtils::get_data_path("images/f03_legacy_rgba.exr")));
}

TEST_CASE("[N029][EXROutputAllocation] Converted output allocation fails atomically after complete input") {
	const Vector<uint8_t> encoded = EXRFixture::fixture("R,G,B,A", false, false);
	const size_t allocation = TestAllocation::allocation_size([]() {
		Vector<uint8_t> output;
		REQUIRE(output.resize(17 * 19 * 4 * 4) == OK);
	});
	const size_t input_allocation = TestAllocation::allocation_size([&]() {
		Vector<uint8_t> source;
		REQUIRE(source.resize(encoded.size()) == OK);
	});
	REQUIRE(allocation != input_allocation);
	Ref<Input> file = input(encoded);
	Ref<Image> destination = sentinel();
	const Vector<uint8_t> before = destination->get_data();
	ImageLoaderTinyEXR loader;
	QuietErrors quiet;
	{
		// Source and destination have distinct exact capacities.
		TestAllocation::Failure failure(allocation);
		CHECK(loader.load_image(destination, file, 0, 1.0) == ERR_OUT_OF_MEMORY);
		CHECK(TestAllocation::failures() == 1);
	}
	CHECK(file->reads == 1);
	CHECK(destination->get_data() == before);
	file->seek(0);
	REQUIRE(loader.load_image(destination, file, 0, 1.0) == OK);
	CHECK(destination->get_size() == Vector2i(17, 19));
}

#endif
#endif
} // namespace TestImageLoaderStaging
