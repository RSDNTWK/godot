/**************************************************************************/
/*  test_allocation_boundaries.cpp                                        */
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

TEST_FORCE_LINK(test_allocation_boundaries)

#include "core/io/image.h"
#include "core/os/memory.h"
#include "core/templates/cowdata.h"
#include "core/templates/vector.h"
#include "tests/core/io/allocation_test_helpers.h"

#include <cstring>

namespace TestAllocationBoundaries {

struct ErrorsOff {
	bool previous = CoreGlobals::print_error_enabled;
	ErrorsOff() { CoreGlobals::print_error_enabled = false; }
	~ErrorsOff() { CoreGlobals::print_error_enabled = previous; }
};

struct Counted {
	static inline int live = 0;
	static inline int constructed = 0;
	uint64_t value = 0;
	Counted() {
		live++;
		constructed++;
	}
	explicit Counted(uint64_t p_value) : value(p_value) {
		live++;
		constructed++;
	}
	Counted(const Counted &p_other) : value(p_other.value) {
		live++;
		constructed++;
	}
	Counted(Counted &&p_other) : value(p_other.value) {
		p_other.value = 0;
		live++;
		constructed++;
	}
	~Counted() { live--; }
};

TEST_CASE("[N014][AllocationBoundary] Typed extents reject overflow for empty exclusive and shared buffers") {
	ErrorsOff quiet;
	const uint64_t impossible = uint64_t(SIZE_MAX) / sizeof(uint64_t) + 1;
	CowData<uint64_t> data;
	CHECK(data.reserve_exact(impossible) == ERR_OUT_OF_MEMORY);
	CHECK(data.ptr() == nullptr);
	CHECK(data.capacity() == 0);
	REQUIRE(data.resize(3) == OK);
	data.set(0, 73);
	const uint64_t *old_pointer = data.ptr();
	const uint64_t old_capacity = data.capacity();
	CHECK(data.reserve_exact(impossible) == ERR_OUT_OF_MEMORY);
	CHECK(data.ptr() == old_pointer);
	CHECK(data.capacity() == old_capacity);
	CHECK(data.size() == 3);
	CHECK(data.get(0) == 73);
	CowData<uint64_t> copy = data;
	CHECK(data.reserve_exact(impossible) == ERR_OUT_OF_MEMORY);
	CHECK(copy.ptr() == old_pointer);
	CHECK(data.ptr() == old_pointer);
	CHECK(data.get(0) == 73);
	CHECK(data.reserve_exact(UINT64_MAX) == ERR_OUT_OF_MEMORY);
	Vector<uint64_t> vector;
	CHECK(vector.resize(INT64_MAX) == ERR_OUT_OF_MEMORY);
	CHECK(vector.is_empty());
}

TEST_CASE("[N014][AllocationBoundary] Array multiplication rejects before constructing elements") {
	ErrorsOff quiet;
	const int before = Counted::constructed;
	CHECK(memnew_arr(Counted, SIZE_MAX / sizeof(Counted) + 1) == nullptr);
	CHECK(Counted::constructed == before);
#if SIZE_MAX < UINT64_MAX
	const uint64_t oversized = uint64_t(SIZE_MAX) + 2;
	CHECK(memnew_arr(Counted, oversized) == nullptr);
	CHECK(Counted::constructed == before);
#endif
	CHECK(memnew_arr(Counted, 0) == nullptr);
	Counted *array = memnew_arr(Counted, 3);
	REQUIRE(array != nullptr);
	CHECK(memarr_len(array) == 3);
	CHECK(Counted::live == 3);
	memdelete_arr(array);
	CHECK(Counted::live == 0);
}

TEST_CASE("[N014][AllocationBoundary] Ordinary growth shrink reserve and COW retain object lifetimes") {
	CHECK(Counted::live == 0);
	{
		CowData<Counted> data;
		REQUIRE(data.reserve_exact(8) == OK);
		REQUIRE(data.push_back(Counted(17)) == OK);
		REQUIRE(data.insert(0, Counted(23)) == OK);
		CowData<Counted> copy = data;
		REQUIRE(data.push_back(Counted(31)) == OK);
		CHECK(copy.size() == 2);
		CHECK(copy.get(0).value == 23);
		CHECK(data.get(2).value == 31);
		REQUIRE(data.resize(1) == OK);
		CHECK(data.get(0).value == 23);
		REQUIRE(data.reserve_exact(1) == OK);
		CHECK(copy.get(1).value == 17);
	}
	CHECK(Counted::live == 0);
}

TEST_CASE("[N015][AllocationBoundary] Raw padded extents reject before metadata changes") {
	ErrorsOff quiet;
	CHECK(Memory::alloc_static(SIZE_MAX, true) == nullptr);
	CHECK(Memory::alloc_static_zeroed(SIZE_MAX, true) == nullptr);
	uint8_t *data = (uint8_t *)Memory::alloc_static(19, true);
	REQUIRE(data != nullptr);
	memset(data, 0x59, 19);
	CHECK(Memory::realloc_static(data, SIZE_MAX, true) == nullptr);
	CHECK(*(uint64_t *)(data - Memory::DATA_OFFSET + Memory::SIZE_OFFSET) == 19);
	for (int i = 0; i < 19; i++) {
		CHECK(data[i] == 0x59);
	}
	Memory::free_static(data, true);
	void *zero = Memory::alloc_static_zeroed(0, true);
	REQUIRE(zero != nullptr);
	CHECK(Memory::realloc_static(zero, 0, true) == nullptr);
}

TEST_CASE("[N015][AllocationBoundary] Aligned shrink grow and zero retain the valid prefix") {
	ErrorsOff quiet;
	CHECK(Memory::alloc_aligned_static(SIZE_MAX, 32) == nullptr);
	for (size_t alignment : { size_t(1), size_t(2), size_t(4), size_t(16), size_t(64), size_t(4096) }) {
		uint8_t *data = (uint8_t *)Memory::alloc_aligned_static(257, alignment);
		REQUIRE(data != nullptr);
		CHECK((uintptr_t(data) % alignment) == 0);
		for (int i = 0; i < 257; i++) {
			data[i] = uint8_t(i);
		}
		uint8_t *shrunk = (uint8_t *)Memory::realloc_aligned_static(data, 13, 257, alignment);
		REQUIRE(shrunk != nullptr);
		CHECK((uintptr_t(shrunk) % alignment) == 0);
		for (int i = 0; i < 13; i++) {
			CHECK(shrunk[i] == i);
		}
		uint8_t *grown = (uint8_t *)Memory::realloc_aligned_static(shrunk, 83, 13, alignment);
		REQUIRE(grown != nullptr);
		for (int i = 0; i < 13; i++) {
			CHECK(grown[i] == i);
		}
		CHECK(Memory::realloc_aligned_static(grown, SIZE_MAX, 83, alignment) == nullptr);
		CHECK(grown[12] == 12);
		void *zero = Memory::realloc_aligned_static(grown, 0, 83, alignment);
		REQUIRE(zero != nullptr);
		Memory::free_aligned_static(zero);
	}
}

TEST_CASE("[N013][ImageInitialization] Pixel limits reject wide products without changing the image") {
	ErrorsOff quiet;
	Ref<Image> image = Image::create_empty(3, 5, false, Image::FORMAT_RGBA8);
	REQUIRE(!image->is_empty());
	image->fill(Color(0.25, 0.5, 0.75));
	const Vector<uint8_t> previous = image->get_data();
	for (Vector2i dimensions : { Vector2i(65536, 65536), Vector2i(Image::MAX_WIDTH, Image::MAX_HEIGHT), Vector2i(0, 1), Vector2i(1, -1) }) {
		image->initialize_data(dimensions.x, dimensions.y, false, Image::FORMAT_RGBA8);
		CHECK(image->get_width() == 3);
		CHECK(image->get_height() == 5);
		CHECK(image->get_data() == previous);
		image->initialize_data(dimensions.x, dimensions.y, false, Image::FORMAT_RGBA8, previous);
		CHECK(image->get_data() == previous);
		image->resize(dimensions.x, dimensions.y);
		CHECK(image->get_width() == 3);
		CHECK(image->get_height() == 5);
		CHECK(image->get_data() == previous);
	}
	image->crop_from_point(INT32_MAX, 0, 1, 1);
	CHECK(image->get_data() == previous);
	image->crop(65536, 65536);
	CHECK(image->get_width() == 3);
	CHECK(image->get_height() == 5);
	CHECK(image->get_data() == previous);
}

TEST_CASE("[N013][ImageInitialization] Valid formats mipmaps and supplied pixels remain usable") {
	for (Image::Format format : { Image::FORMAT_L8, Image::FORMAT_RGB8, Image::FORMAT_RGBA8, Image::FORMAT_RGBAF }) {
		for (bool mipmaps : { false, true }) {
			Ref<Image> image = Image::create_empty(17, 19, mipmaps, format);
			REQUIRE(!image->is_empty());
			CHECK(image->get_width() == 17);
			CHECK(image->get_height() == 19);
			CHECK(image->has_mipmaps() == mipmaps);
			const Vector<uint8_t> pixels = image->get_data();
			for (uint8_t value : pixels) {
				CHECK(value == 0);
			}
			Ref<Image> copy = Image::create_from_data(17, 19, mipmaps, format, pixels);
			CHECK(copy->get_data() == pixels);
			CHECK(copy->get_format() == format);
		}
	}
}

#ifdef B01_ALLOCATION_TESTS
TEST_CASE("[N014][AllocationFailure] Initial insert and push preserve the unconsumed object") {
	ErrorsOff quiet;
	for (bool insert : { false, true }) {
		const size_t exact = TestAllocation::allocation_size([] {
			CowData<Counted> probe;
			probe.push_back(Counted(42));
		});
		REQUIRE(exact != 0);
		CowData<Counted> data;
		Counted value(91);
		Error error;
		{
			TestAllocation::Failure fault(exact);
			error = insert ? data.insert(0, std::move(value)) : data.push_back(std::move(value));
		}
		CHECK(TestAllocation::failures() == 1);
		CHECK(error == ERR_OUT_OF_MEMORY);
		CHECK(value.value == 91);
		CHECK(data.ptr() == nullptr);
		CHECK(data.size() == 0);
		REQUIRE(data.push_back(std::move(value)) == OK);
		CHECK(data.get(0).value == 91);
	}
	CHECK(Counted::live == 0);
}

TEST_CASE("[N014][AllocationFailure] Existing exclusive and shared buffers retain ownership on failure") {
	ErrorsOff quiet;
	for (bool shared : { false, true }) {
		CowData<uint64_t> data;
		REQUIRE(data.resize(3) == OK);
		data.set(0, 73);
		CowData<uint64_t> copy;
		if (shared) {
			copy = data;
		}
		const uint64_t *old_pointer = data.ptr();
		const uint64_t old_capacity = data.capacity();
		const size_t exact = TestAllocation::allocation_size([] {
			CowData<uint64_t> probe;
			probe.reserve_exact(127);
		});
		Error error;
		{
			TestAllocation::Failure fault(exact);
			error = data.reserve_exact(127);
		}
		CHECK(TestAllocation::failures() == 1);
		CHECK(error == ERR_OUT_OF_MEMORY);
		CHECK(data.ptr() == old_pointer);
		CHECK(data.capacity() == old_capacity);
		CHECK(data.size() == 3);
		CHECK(data.get(0) == 73);
		CHECK(shared ? copy.ptr() == old_pointer : copy.ptr() == nullptr);
		REQUIRE(data.reserve_exact(127) == OK);
		CHECK(data.get(0) == 73);
	}
}

TEST_CASE("[N015][AllocationFailure] Padded realloc failures preserve bytes size and accounting") {
	ErrorsOff quiet;
	uint8_t *data = (uint8_t *)Memory::alloc_static(73, true);
	REQUIRE(data != nullptr);
	memset(data, 0x71, 73);
	const uint64_t previous_usage = Memory::get_mem_usage();
	void *replacement;
	uint64_t failed_usage;
	{
		TestAllocation::Failure fault(509 + Memory::DATA_OFFSET);
		replacement = Memory::realloc_static(data, 509, true);
		failed_usage = Memory::get_mem_usage();
	}
	CHECK(TestAllocation::failures() == 1);
	CHECK(replacement == nullptr);
	CHECK(failed_usage == previous_usage);
	CHECK(*(uint64_t *)(data - Memory::DATA_OFFSET + Memory::SIZE_OFFSET) == 73);
	for (int i = 0; i < 73; i++) {
		CHECK(data[i] == 0x71);
	}
	data = (uint8_t *)Memory::realloc_static(data, 509, true);
	REQUIRE(data != nullptr);
	CHECK(*(uint64_t *)(data - Memory::DATA_OFFSET + Memory::SIZE_OFFSET) == 509);
	CHECK(data[72] == 0x71);
	Memory::free_static(data, true);
}

TEST_CASE("[N015][AllocationFailure] Aligned replacement failure keeps the original freeable") {
	const size_t exact = TestAllocation::allocation_size([] {
		void *probe = Memory::alloc_aligned_static(509, 64);
		Memory::free_aligned_static(probe);
	});
	REQUIRE(exact != 0);
	uint8_t *data = (uint8_t *)Memory::alloc_aligned_static(73, 64);
	REQUIRE(data != nullptr);
	memset(data, 0x71, 73);
	void *replacement;
	{
		TestAllocation::Failure fault(exact);
		replacement = Memory::realloc_aligned_static(data, 509, 73, 64);
	}
	CHECK(TestAllocation::failures() == 1);
	CHECK(replacement == nullptr);
	for (int i = 0; i < 73; i++) {
		CHECK(data[i] == 0x71);
	}
	Memory::free_aligned_static(data);
}

TEST_CASE("[N013][ImageInitializationAllocation] Failed fresh growth and same-size shared initialization preserve state") {
	ErrorsOff quiet;
	for (int mode : { 0, 1, 2 }) {
		Ref<Image> image;
		image.instantiate();
		if (mode != 0) {
			image->initialize_data(3, 5, false, Image::FORMAT_RGBA8);
			image->fill(Color(0.25, 0.5, 0.75));
		}
		const Vector<uint8_t> previous = image->get_data();
		const int old_width = image->get_width();
		const int old_height = image->get_height();
		const int width = mode == 2 ? 3 : 17;
		const int height = mode == 2 ? 5 : 19;
		const size_t exact = TestAllocation::allocation_size([&] {
			Vector<uint8_t> probe;
			probe.resize(width * height * 4);
		});
		{
			TestAllocation::Failure fault(exact);
			image->initialize_data(width, height, false, Image::FORMAT_RGBA8);
		}
		CHECK(TestAllocation::failures() == 1);
		CHECK(image->get_width() == old_width);
		CHECK(image->get_height() == old_height);
		CHECK(image->get_data() == previous);
		image->initialize_data(width, height, false, Image::FORMAT_RGBA8);
		CHECK(image->get_width() == width);
		CHECK(image->get_height() == height);
		for (uint8_t value : image->get_data()) {
			CHECK(value == 0);
		}
	}
}

TEST_CASE("[N013][ImageCallerAllocation] Resize and crop failures preserve converted source formats") {
	ErrorsOff quiet;
	for (Image::Format format : { Image::FORMAT_RGBA8, Image::FORMAT_RGB565, Image::FORMAT_RGBE9995 }) {
		for (bool crop : { false, true }) {
			Ref<Image> image = Image::create_empty(3, 5, false, format);
			image->fill(Color(0.25, 0.5, 0.75));
			const Vector<uint8_t> previous = image->get_data();
			Image::Format destination_format = format;
			if (!crop && format == Image::FORMAT_RGB565) {
				destination_format = Image::FORMAT_RGBA8;
			} else if (!crop && format == Image::FORMAT_RGBE9995) {
				destination_format = Image::FORMAT_RGBH;
			}
			const size_t exact = TestAllocation::allocation_size([&] {
				Vector<uint8_t> probe;
				probe.resize(17 * 19 * Image::get_format_pixel_size(destination_format));
			});
			{
				TestAllocation::Failure fault(exact);
				if (crop) {
					image->crop(17, 19);
				} else {
					image->resize(17, 19, Image::INTERPOLATE_NEAREST);
				}
			}
			CHECK(TestAllocation::failures() == 1);
			CHECK(image->get_width() == 3);
			CHECK(image->get_height() == 5);
			CHECK(image->get_format() == format);
			CHECK(image->get_data() == previous);
			if (crop) {
				image->crop(17, 19);
			} else {
				image->resize(17, 19, Image::INTERPOLATE_NEAREST);
			}
			CHECK(image->get_width() == 17);
			CHECK(image->get_height() == 19);
			CHECK(image->get_format() == format);
		}
	}
}

TEST_CASE("[N013][ImageCallerAllocation] Second trilinear destination failure leaves mipmaps intact") {
	ErrorsOff quiet;
	Ref<Image> image = Image::create_empty(64, 64, true, Image::FORMAT_RGBA8);
	image->fill(Color(0.25, 0.5, 0.75));
	const Vector<uint8_t> previous = image->get_data();
	const size_t exact = TestAllocation::allocation_size([] {
		Vector<uint8_t> probe;
		probe.resize(17 * 19 * 4);
	});
	{
		TestAllocation::Failure fault(exact, 2);
		image->resize(17, 19, Image::INTERPOLATE_TRILINEAR);
	}
	CHECK(TestAllocation::failures() == 1);
	CHECK(image->get_width() == 64);
	CHECK(image->get_height() == 64);
	CHECK(image->has_mipmaps());
	CHECK(image->get_data() == previous);
	image->resize(17, 19, Image::INTERPOLATE_TRILINEAR);
	CHECK(image->get_width() == 17);
	CHECK(image->get_height() == 19);
	CHECK(image->has_mipmaps());
}

TEST_CASE("[N013][ImageCallerAllocation] Compatible and general conversion failures preserve their source") {
	ErrorsOff quiet;
	for (Image::Format format : { Image::FORMAT_RGB8, Image::FORMAT_RGBAF }) {
		Ref<Image> image = Image::create_empty(17, 19, false, Image::FORMAT_RGBA8);
		image->fill(Color(0.25, 0.5, 0.75));
		const Vector<uint8_t> previous = image->get_data();
		const size_t exact = TestAllocation::allocation_size([&] {
			Vector<uint8_t> probe;
			probe.resize(17 * 19 * Image::get_format_pixel_size(format));
		});
		{
			TestAllocation::Failure fault(exact);
			image->convert(format);
		}
		CHECK(TestAllocation::failures() == 1);
		CHECK(image->get_format() == Image::FORMAT_RGBA8);
		CHECK(image->get_data() == previous);
		image->convert(format);
		CHECK(image->get_format() == format);
		CHECK(image->get_width() == 17);
		CHECK(image->get_height() == 19);
	}
}

TEST_CASE("[N013][ImageCallerAllocation] Failed XPM initialization preserves a smaller existing image") {
	ErrorsOff quiet;
	const char *xpm[21];
	xpm[0] = "17 19 1 1";
	xpm[1] = ". c #204060";
	for (int i = 2; i < 21; i++) {
		xpm[i] = ".................";
	}
	for (bool mipmaps : { false, true }) {
		Ref<Image> image = Image::create_empty(1, 1, mipmaps, Image::FORMAT_L8);
		image->fill(Color(0.5, 0.5, 0.5));
		const Vector<uint8_t> previous = image->get_data();
		const size_t exact = TestAllocation::allocation_size([] {
			Vector<uint8_t> probe;
			probe.resize(17 * 19 * 3);
		});
		{
			TestAllocation::Failure fault(exact);
			image->initialize_data(xpm);
		}
		CHECK(TestAllocation::failures() == 1);
		CHECK(image->get_width() == 1);
		CHECK(image->get_height() == 1);
		CHECK(image->get_format() == Image::FORMAT_L8);
		CHECK(image->has_mipmaps() == mipmaps);
		CHECK(image->get_data() == previous);
		image->initialize_data(xpm);
		CHECK(image->get_width() == 17);
		CHECK(image->get_height() == 19);
		CHECK(image->get_format() == Image::FORMAT_RGB8);
		CHECK_FALSE(image->has_mipmaps());
		CHECK(image->get_pixel(0, 0).r == doctest::Approx(32.0 / 255.0));
		CHECK(image->get_pixel(16, 18).b == doctest::Approx(96.0 / 255.0));
	}
}

#endif

} // namespace TestAllocationBoundaries
