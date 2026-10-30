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
TEST_FORCE_LINK(test_exr_channel_types)
#include "modules/modules_enabled.gen.h"
#ifdef MODULE_TINYEXR_ENABLED
#include "tests/core/io/exr_test_helpers.h"

namespace TestEXRChannelTypes {
TEST_CASE("[N040][EXRChannelTypes] UINT channels convert numerically for tiled and scanline images") {
	for (bool tiled : { false, true }) {
		for (const char *names : { "R", "Y", "R,G,B,A" }) {
			for (uint32_t value : { uint32_t(1), uint32_t(0x7f800000) }) {
				const Vector<uint8_t> encoded = TestEXRFixture::fixture(names, false, tiled, false, names, value);
				Ref<Image> image;
				image.instantiate();
				REQUIRE(TestEXRFixture::load(encoded, image) == OK);
				const Color pixel = image->get_pixel(0, 0);
				CHECK(Math::is_finite(pixel.r));
				CHECK(pixel.r == float(value));
				if (String(names) != "R") {
					CHECK(pixel.g == float(value));
					CHECK(pixel.b == float(value));
				}
				if (String(names) == "R,G,B,A") {
					CHECK(pixel.a == float(value));
				}
			}
		}
	}
}
TEST_CASE("[N040][EXRChannelTypes] Mixed integer float and half channels preserve output policy") {
	for (bool tiled : { false, true }) {
		for (bool half : { false, true }) {
			const Vector<uint8_t> encoded = TestEXRFixture::fixture("R,G,B,A", half, tiled, false, "R,B", 1);
			Ref<Image> image;
			image.instantiate();
			REQUIRE(TestEXRFixture::load(encoded, image) == OK);
			CHECK(image->get_format() == (half ? Image::FORMAT_RGBAH : Image::FORMAT_RGBAF));
			const Color pixel = image->get_pixel(0, 0);
			CHECK(pixel.r == 1.0f);
			CHECK(pixel.g == 0.25f);
			CHECK(pixel.b == 1.0f);
			CHECK(pixel.a == 0.5f);
			REQUIRE(TestEXRFixture::load(encoded, image, ImageFormatLoader::FLAG_FORCE_LINEAR) == OK);
			CHECK(image->get_pixel(0, 0).is_equal_approx(pixel.srgb_to_linear()));
		}
	}
}
} //namespace TestEXRChannelTypes
#endif
