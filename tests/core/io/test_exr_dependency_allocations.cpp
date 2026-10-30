/**************************************************************************/
/*  test_exr_dependency_allocations.cpp                                   */
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
TEST_FORCE_LINK(test_exr_dependency_allocations)
#include "modules/modules_enabled.gen.h"
#ifdef MODULE_TINYEXR_ENABLED
#include "thirdparty/tinyexr/tinyexr.h"

#include "tests/core/io/allocation_test_helpers.h"
#include "tests/core/io/exr_test_helpers.h"

namespace TestEXRDependencyAllocations {
TEST_CASE("[N035][EXRDependency] Custom attribute and multi-channel fixtures preserve valid loading") {
	for (bool tiled : { false, true }) {
		Ref<Image> image;
		image.instantiate();
		REQUIRE(TestEXRFixture::load(TestEXRFixture::fixture("R,G,B,A", false, tiled, false, "", 1, 2, 67), image) == OK);
		CHECK(image->get_pixel(0, 0).is_equal_approx(Color(0.125f, 0.25f, 0.375f, 0.5f)));
	}
}
#ifdef B01_ALLOCATION_TESTS
struct QuietErrors {
	bool previous = CoreGlobals::print_error_enabled;
	QuietErrors() { CoreGlobals::print_error_enabled = false; }
	~QuietErrors() { CoreGlobals::print_error_enabled = previous; }
};
static void failed_load(const Vector<uint8_t> &p_encoded, size_t p_bytes, int p_occurrence = 1) {
	Ref<Image> image = Image::create_empty(2, 3, false, Image::FORMAT_RGBA8);
	image->fill(Color(0.25, 0.5, 0.75));
	const Vector<uint8_t> before = image->get_data();
	QuietErrors quiet;
	{
		TestAllocation::Failure failure(p_bytes, p_occurrence);
		CHECK(TestEXRFixture::load(p_encoded, image) != OK);
		CHECK(TestAllocation::failures() == 1);
	}
	CHECK(image->get_size() == Vector2i(2, 3));
	CHECK(image->get_data() == before);
	REQUIRE(TestEXRFixture::load(p_encoded, image) == OK);
	CHECK(image->get_size() == Vector2i(17, 19));
}
TEST_CASE("[N035][EXRDependencyAllocation] Header tables custom values and partial planes fail safely") {
	const Vector<uint8_t> plain = TestEXRFixture::fixture("R,G,B,A", false, false);
	failed_load(plain, sizeof(EXRChannelInfo) * 4);
	failed_load(plain, sizeof(int) * 4, 1);
	failed_load(plain, sizeof(int) * 4, 2);
	failed_load(plain, sizeof(unsigned char *) * 4);
	for (int occurrence = 1; occurrence <= 4; occurrence++) {
		failed_load(plain, size_t(17 * 19 * sizeof(float)), occurrence);
	}
	const Vector<uint8_t> custom = TestEXRFixture::fixture("R,G,B,A", false, false, false, "", 1, 2, 67);
	failed_load(custom, 67);
	failed_load(custom, sizeof(EXRAttribute));
}
TEST_CASE("[N035][EXRDependencyAllocation] Tiled image table failure returns before starting workers") {
	failed_load(TestEXRFixture::fixture("R,G,B,A", false, true), size_t(9 * 10) * sizeof(EXRTile));
}
#endif
} //namespace TestEXRDependencyAllocations
#endif
