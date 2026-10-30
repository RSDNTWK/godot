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
TEST_FORCE_LINK(test_image_mipmap_safety)
#include "core/io/image.h"
#include "core/io/image_mipmap_layout.h"
#include "tests/core/io/allocation_test_helpers.h"

namespace TestImageMipmapSafety {
TEST_CASE("[N013][MipmapSafety] Production layout retains offsets beyond signed int without allocating images") {
	ImageMipmapLayout::Layout layout;
	REQUIRE(ImageMipmapLayout::calculate(16384, 8192, 16, layout) == OK);
	CHECK(layout.base_bytes == INT64_C(2147483648));
	CHECK(layout.total_bytes == INT64_C(2863311536));
	REQUIRE(layout.count > 1);
	CHECK(layout.levels[0].destination_offset == INT64_C(2147483648));
	CHECK(layout.levels[1].source_offset == INT64_C(2147483648));
	for (int i = 0; i < layout.count; i++) {
		const auto &level = layout.levels[i];
		CHECK(level.source_offset >= 0);
		CHECK(level.source_offset < level.destination_offset);
		CHECK(level.destination_offset < layout.total_bytes);
		CHECK(level.destination_offset == Image::get_image_mipmap_offset(16384, 8192, Image::FORMAT_RGBAF, i + 1));
	}
	CHECK(ImageMipmapLayout::calculate(INT_MAX, INT_MAX, INT_MAX, layout) == ERR_OUT_OF_MEMORY);
}
TEST_CASE("[N013][MipmapSafety] Small mip chains preserve uniform pixels and supported layouts") {
	for (Image::Format format : { Image::FORMAT_L8, Image::FORMAT_RGBA8, Image::FORMAT_RGBH, Image::FORMAT_RGBAF }) {
		for (Vector2i size : { Vector2i(3, 5), Vector2i(8, 4), Vector2i(1, 7), Vector2i(1, 1) }) {
			Ref<Image> image = Image::create_empty(size.x, size.y, false, format);
			image->fill(Color(0.125, 0.25, 0.5, 1));
			const Color expected = image->get_pixel(0, 0);
			REQUIRE(image->generate_mipmaps() == OK);
			CHECK(image->get_data().size() == Image::get_image_data_size(size.x, size.y, format, true));
			const Ref<Image> last = image->get_image_from_mipmap(image->get_mipmap_count());
			REQUIRE(last.is_valid());
			CHECK(last->get_pixel(0, 0).is_equal_approx(expected));
		}
	}
}
#ifdef B01_ALLOCATION_TESTS
struct QuietErrors {
	bool previous = CoreGlobals::print_error_enabled;
	QuietErrors() { CoreGlobals::print_error_enabled = false; }
	~QuietErrors() { CoreGlobals::print_error_enabled = previous; }
};
static size_t chain_allocation(int p_width, int p_height) {
	return TestAllocation::allocation_size([&]() {
		Vector<uint8_t> bytes;
		REQUIRE(bytes.resize(Image::get_image_data_size(p_width, p_height, Image::FORMAT_RGBA8, true)) == OK);
	});
}
TEST_CASE("[N013][MipmapSafetyAllocation] Failed fresh and shared same-size generation preserve exact state") {
	for (bool mipmapped : { false, true }) {
		Ref<Image> image = Image::create_empty(3, 5, false, Image::FORMAT_RGBA8);
		image->fill(Color(0.125, 0.25, 0.5, 1));
		if (mipmapped) {
			REQUIRE(image->generate_mipmaps() == OK);
		}
		const Vector<uint8_t> shared = image->get_data();
		const size_t allocation = chain_allocation(3, 5);
		QuietErrors quiet;
		{
			TestAllocation::Failure failure(allocation);
			CHECK(image->generate_mipmaps() == ERR_OUT_OF_MEMORY);
			CHECK(TestAllocation::failures() == 1);
		}
		CHECK(image->has_mipmaps() == mipmapped);
		CHECK(image->get_data() == shared);
		REQUIRE(image->generate_mipmaps() == OK);
		CHECK(shared.size() == Image::get_image_data_size(3, 5, Image::FORMAT_RGBA8, mipmapped));
	}
}
TEST_CASE("[N013][MipmapSafetyAllocation] Resize and crop reject failed mip generation before publishing") {
	for (bool crop : { false, true }) {
		Ref<Image> image = Image::create_empty(64, 64, false, Image::FORMAT_RGBA8);
		image->fill(Color(0.125, 0.25, 0.5, 1));
		REQUIRE(image->generate_mipmaps() == OK);
		const Vector<uint8_t> before = image->get_data();
		const size_t allocation = chain_allocation(17, 19);
		QuietErrors quiet;
		{
			TestAllocation::Failure failure(allocation);
			if (crop) {
				image->crop(17, 19);
			} else {
				image->resize(17, 19);
			}
			CHECK(TestAllocation::failures() == 1);
		}
		CHECK(image->get_size() == Vector2i(64, 64));
		CHECK(image->has_mipmaps());
		CHECK(image->get_data() == before);
		if (crop) {
			image->crop(17, 19);
		} else {
			image->resize(17, 19);
		}
		CHECK(image->get_size() == Vector2i(17, 19));
		CHECK(image->has_mipmaps());
	}
}
TEST_CASE("[N013][MipmapSafetyAllocation] Clear and transforming callers preserve state at either allocation failure") {
	for (int operation = 0; operation < 6; operation++) {
		for (bool regeneration : { false, true }) {
			if (operation == 0 && regeneration) {
				continue;
			}
			Ref<Image> image = Image::create_empty(8, 4, false, Image::FORMAT_RGBA8);
			image->fill(Color(0.125, 0.25, 0.5, 1));
			REQUIRE(image->generate_mipmaps() == OK);
			const Vector<uint8_t> before = image->get_data();
			const auto transform = [&]() {
				switch (operation) {
					case 0:
						image->clear_mipmaps();
						break;
					case 1:
						image->rotate_90(CLOCKWISE);
						break;
					case 2:
						image->rotate_180();
						break;
					case 3:
						image->flip_x();
						break;
					case 4:
						image->flip_y();
						break;
					case 5:
						image->normalize();
						break;
				}
			};
			const size_t allocation = TestAllocation::allocation_size([&]() {
				Vector<uint8_t> bytes;
				REQUIRE(bytes.resize(Image::get_image_data_size(8, 4, Image::FORMAT_RGBA8, regeneration)) == OK);
			});
			QuietErrors quiet;
			{
				TestAllocation::Failure failure(allocation);
				transform();
				CHECK(TestAllocation::failures() == 1);
			}
			CHECK(image->get_size() == Vector2i(8, 4));
			CHECK(image->has_mipmaps());
			CHECK(image->get_data() == before);
			transform();
			CHECK(image->has_mipmaps() == (operation != 0));
			CHECK(image->get_size() == (operation == 1 ? Vector2i(4, 8) : Vector2i(8, 4)));
		}
	}
}
#endif
} //namespace TestImageMipmapSafety
