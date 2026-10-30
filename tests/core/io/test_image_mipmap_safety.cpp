/**************************************************************************/
/*  test_image_mipmap_safety.cpp                                          */
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
#include "editor/import/texture_mipmap_preparation.h"
#include "tests/core/io/allocation_test_helpers.h"
#ifdef TOOLS_ENABLED
#include "core/io/dir_access.h"
#include "core/os/os.h"
#include "editor/import/resource_importer_layered_texture.h"
#include "scene/resources/compressed_texture.h"
#include "tests/test_utils.h"
#endif

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
TEST_CASE("[N013][MipmapSafety] Bump conversion publishes a valid base-only normal image") {
	for (Image::Format format : { Image::FORMAT_RF, Image::FORMAT_RGB8 }) {
		for (bool mipmapped : { false, true }) {
			Ref<Image> image = Image::create_empty(4, 4, mipmapped, format);
			image->fill(Color(0.5, 0.5, 0.5, 1));
			image->bump_map_to_normal_map();
			CHECK(image->get_format() == Image::FORMAT_RGBA8);
			CHECK_FALSE(image->has_mipmaps());
			CHECK(image->get_data().size() == 64);
			CHECK(image->get_pixel(0, 0).is_equal_approx(Color(127.0 / 255.0, 127.0 / 255.0, 1, 1)));
		}
	}
}
static Vector<Ref<Image>> layers(bool p_mipmaps = false) {
	Vector<Ref<Image>> result;
	REQUIRE(result.resize(2) == OK);
	for (int i = 0; i < result.size(); i++) {
		result.write[i] = Image::create_empty(4, 4, p_mipmaps, Image::FORMAT_RGBA8);
		result[i]->fill(Color(0.125, 0.25, 0.5, 1));
	}
	return result;
}
TEST_CASE("[N013][MipmapSafety] Production layered preparation preserves 2D and volume controls") {
	for (bool volume : { false, true }) {
		for (bool mipmaps : { false, true }) {
			Vector<Ref<Image>> images = layers();
			Vector<Ref<Image>> generated;
			REQUIRE(TextureMipmapPreparation::prepare_layers(images, generated, volume, mipmaps, false, false) == OK);
			for (const Ref<Image> &image : images) {
				CHECK(image->has_mipmaps() == (!volume && mipmaps));
				CHECK(image->get_size() == Vector2i(4, 4));
			}
			CHECK(generated.size() == (volume && mipmaps ? 2 : 0));
			if (!generated.is_empty()) {
				CHECK(generated[0]->get_size() == Vector2i(2, 2));
				CHECK(generated[1]->get_size() == Vector2i(1, 1));
				CHECK(generated[1]->get_pixel(0, 0).is_equal_approx(images[0]->get_pixel(0, 0)));
			}
		}
	}
	Vector<Ref<Image>> images = layers(true);
	Vector<Ref<Image>> generated;
	REQUIRE(TextureMipmapPreparation::prepare_layers(images, generated, false, false, false, false) == OK);
	CHECK_FALSE(images[0]->has_mipmaps());
	CHECK_FALSE(images[1]->has_mipmaps());
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
TEST_CASE("[N013][MipmapSafetyAllocation] Bump clear conversion and result failures preserve the original") {
	for (int scenario = 0; scenario < 4; scenario++) {
		const bool mipmaps = scenario == 1 || scenario == 2;
		const Image::Format format = scenario == 3 ? Image::FORMAT_RGB8 : Image::FORMAT_RF;
		Ref<Image> image = Image::create_empty(4, 4, mipmaps, format);
		image->fill(Color(0.5, 0.5, 0.5, 1));
		const Vector<uint8_t> before = image->get_data();
		const size_t allocation = TestAllocation::allocation_size([]() {
			Vector<uint8_t> bytes;
			REQUIRE(bytes.resize(64) == OK);
		});
		QuietErrors quiet;
		{
			TestAllocation::Failure injected(allocation, scenario == 2 ? 2 : 1);
			image->bump_map_to_normal_map();
			CHECK(TestAllocation::failures() == 1);
		}
		CHECK(image->get_format() == format);
		CHECK(image->has_mipmaps() == mipmaps);
		CHECK(image->get_data() == before);
		CHECK(image->get_size() == Vector2i(4, 4));
		image->bump_map_to_normal_map();
		CHECK(image->get_format() == Image::FORMAT_RGBA8);
		CHECK_FALSE(image->has_mipmaps());
		CHECK(image->get_data().size() == 64);
	}
}
TEST_CASE("[N013][MipmapSafetyAllocation] Layered preparation propagates failed generation even for existing chains") {
	for (bool mipmapped : { false, true }) {
		Vector<Ref<Image>> images = layers(mipmapped);
		const Vector<uint8_t> before = images[1]->get_data();
		const Ref<Image> sentinel = Image::create_empty(1, 1, false, Image::FORMAT_RGBA8);
		Vector<Ref<Image>> generated = { sentinel };
		const size_t allocation = chain_allocation(4, 4);
		QuietErrors quiet;
		{
			TestAllocation::Failure injected(allocation, 2);
			CHECK(TextureMipmapPreparation::prepare_layers(images, generated, false, true, false, false) == ERR_OUT_OF_MEMORY);
			CHECK(TestAllocation::failures() == 1);
		}
		CHECK(images[0]->has_mipmaps());
		CHECK(images[1]->has_mipmaps() == mipmapped);
		CHECK(images[1]->get_data() == before);
		REQUIRE(generated.size() == 1);
		CHECK(generated[0] == sentinel);
		REQUIRE(TextureMipmapPreparation::prepare_layers(images, generated, false, true, false, false) == OK);
		CHECK(images[0]->has_mipmaps());
		CHECK(images[1]->has_mipmaps());
		CHECK(generated.is_empty());
	}
}
TEST_CASE("[N013][MipmapSafetyAllocation] Layered clear failures propagate before serialization") {
	Vector<Ref<Image>> images = layers(true);
	const Vector<uint8_t> before = images[1]->get_data();
	Vector<Ref<Image>> generated;
	const size_t allocation = TestAllocation::allocation_size([]() {
		Vector<uint8_t> bytes;
		REQUIRE(bytes.resize(64) == OK);
	});
	QuietErrors quiet;
	{
		TestAllocation::Failure injected(allocation, 2);
		CHECK(TextureMipmapPreparation::prepare_layers(images, generated, false, false, false, false) == ERR_OUT_OF_MEMORY);
		CHECK(TestAllocation::failures() == 1);
	}
	CHECK_FALSE(images[0]->has_mipmaps());
	CHECK(images[1]->has_mipmaps());
	CHECK(images[1]->get_data() == before);
	REQUIRE(TextureMipmapPreparation::prepare_layers(images, generated, false, false, false, false) == OK);
	CHECK_FALSE(images[1]->has_mipmaps());
}
TEST_CASE("[N013][MipmapSafetyAllocation] Volume mipmap plane and table failures leave generated output unpublished") {
	const size_t allocation = TestAllocation::allocation_size([]() {
		Vector<Ref<Image>> one;
		REQUIRE(one.resize(1) == OK);
	});
	const size_t plane_allocation = TestAllocation::allocation_size([]() {
		Vector<uint8_t> plane;
		REQUIRE(plane.resize(16) == OK);
	});
	REQUIRE(allocation == plane_allocation);
	for (int occurrence = 1; occurrence <= 3; occurrence++) {
		Vector<Ref<Image>> images = layers();
		const Ref<Image> sentinel = Image::create_empty(1, 1, false, Image::FORMAT_RGBA8);
		Vector<Ref<Image>> generated = { sentinel };
		QuietErrors quiet;
		{
			TestAllocation::Failure injected(allocation, occurrence);
			CHECK(TextureMipmapPreparation::prepare_layers(images, generated, true, true, false, false) == ERR_OUT_OF_MEMORY);
			CHECK(TestAllocation::failures() == 1);
		}
		REQUIRE(generated.size() == 1);
		CHECK(generated[0] == sentinel);
		REQUIRE(TextureMipmapPreparation::prepare_layers(images, generated, true, true, false, false) == OK);
		CHECK(generated.size() == 2);
	}
}
#endif
#ifdef TOOLS_ENABLED
struct LayeredFixture {
	String path = TestUtils::get_temp_path("n013_layered_" + uitos(OS::get_singleton()->get_ticks_usec()) + ".ctexarray");
	~LayeredFixture() { DirAccess::remove_absolute(path); }
};
TEST_CASE("[N013][MipmapSafetyEditor] Layered preparation failure preserves an existing output") {
	LayeredFixture fixture;
	{
		Ref<FileAccess> file = FileAccess::open(fixture.path, FileAccess::WRITE);
		REQUIRE(file.is_valid());
		REQUIRE(file->store_string("retained"));
	}
	Vector<uint8_t> encoded;
	REQUIRE(encoded.resize(8) == OK);
	encoded.fill(0);
	Vector<Ref<Image>> images = { Image::create_from_data(4, 4, false, Image::FORMAT_DXT1, encoded) };
	REQUIRE(images[0].is_valid());
	ResourceImporterLayeredTexture importer;
	importer.set_mode(ResourceImporterLayeredTexture::MODE_2D_ARRAY);
	const bool previous = CoreGlobals::print_error_enabled;
	CoreGlobals::print_error_enabled = false;
	importer._save_tex(images, fixture.path, ResourceImporterLayeredTexture::COMPRESS_VRAM_UNCOMPRESSED, 1, Image::BasisUniversalPackerParams(), Image::COMPRESS_S3TC, Image::COMPRESS_SOURCE_GENERIC, Image::USED_CHANNELS_RGBA, true, false, Image::BPTC_DETECT);
	CoreGlobals::print_error_enabled = previous;
	Ref<FileAccess> file = FileAccess::open(fixture.path, FileAccess::READ);
	REQUIRE(file.is_valid());
	CHECK(file->get_as_utf8_string() == "retained");
}
TEST_CASE("[N013][MipmapSafetyEditor] Layered serialized images retain consistent mipmap flags") {
	for (bool mipmaps : { false, true }) {
		LayeredFixture fixture;
		Vector<Ref<Image>> images = layers();
		ResourceImporterLayeredTexture importer;
		importer.set_mode(ResourceImporterLayeredTexture::MODE_2D_ARRAY);
		importer._save_tex(images, fixture.path, ResourceImporterLayeredTexture::COMPRESS_VRAM_UNCOMPRESSED, 1, Image::BasisUniversalPackerParams(), Image::COMPRESS_S3TC, Image::COMPRESS_SOURCE_GENERIC, Image::USED_CHANNELS_RGBA, mipmaps, false, Image::BPTC_DETECT);
		Ref<FileAccess> file = FileAccess::open(fixture.path, FileAccess::READ);
		REQUIRE(file.is_valid());
		CHECK(file->get_32() == UINT32_C(0x4c545347));
		file->get_32();
		CHECK(file->get_32() == 2);
		CHECK(file->get_32() == ResourceImporterLayeredTexture::MODE_2D_ARRAY);
		file->seek(36);
		for (int i = 0; i < 2; i++) {
			const Ref<Image> image = CompressedTexture2D::load_image_from_file(file, 0);
			REQUIRE(image.is_valid());
			CHECK(image->get_size() == Vector2i(4, 4));
			CHECK(image->has_mipmaps() == mipmaps);
			CHECK(image->get_pixel(0, 0).is_equal_approx(images[i]->get_pixel(0, 0)));
		}
	}
}
#endif
} //namespace TestImageMipmapSafety
