/**************************************************************************/
/*  test_webp_import_validation.cpp                                       */
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
TEST_FORCE_LINK(test_webp_import_validation)

#include "modules/modules_enabled.gen.h"
#ifdef MODULE_WEBP_ENABLED
#include "thirdparty/libwebp/src/webp/demux.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/marshalls.h"
#include "editor/import/resource_importer_webp_texture.h"
#include "tests/core/io/webp_test_helpers.h"

#include "modules/webp/texture_loader_webp.h"

#include <cstring>

namespace TestWebPImportValidation {

static Vector<uint8_t> corrupt_late_frame() {
	Vector<uint8_t> data = TestWebPSafety::make_animation(3);
	int frame = 0;
	bool changed = false;
	for (int64_t offset = 12; offset + 8 <= data.size();) {
		const uint32_t length = decode_uint32(data.ptr() + offset + 4);
		REQUIRE(offset + 8 + length <= data.size());
		if (memcmp(data.ptr() + offset, "ANMF", 4) == 0 && ++frame == 3) {
			for (int64_t chunk = offset + 24; chunk + 8 <= offset + 8 + length;) {
				const uint32_t size = decode_uint32(data.ptr() + chunk + 4);
				REQUIRE(chunk + 8 + size <= offset + 8 + length);
				if (memcmp(data.ptr() + chunk, "VP8L", 4) == 0) {
					REQUIRE(size > 5);
					// Preserve the lossless header and dimensions; invalidate the entropy data.
					memset(data.ptrw() + chunk + 13, 0xff, size - 5);
					changed = true;
				}
				chunk += 8 + size + (size & 1);
			}
		}
		offset += 8 + length + (length & 1);
	}
	REQUIRE(changed);
	return data;
}

TEST_CASE("[E03][WebP] Import validation accepts static and supported animations without a renderer") {
	for (bool alpha : { false, true }) {
		TestWebPSafety::Fixture fixture(TestWebPSafety::make_static(alpha));
		bool animated = true;
		CHECK(ResourceFormatWebP::validate_texture_source(fixture.path, animated) == OK);
		CHECK_FALSE(animated);
	}
	for (int frames : { 1, 2, 64, 256 }) {
		TestWebPSafety::Fixture fixture(TestWebPSafety::make_animation(frames));
		bool animated = false;
		CHECK(ResourceFormatWebP::validate_texture_source(fixture.path, animated) == OK);
		CHECK(animated == (frames > 1));
	}
}

TEST_CASE("[E03][WebP] Import validation preserves frame limits and rejects truncated input") {
	TestWebPSafety::Fixture unsupported(TestWebPSafety::make_animation(257));
	bool animated = true;
	CHECK(ResourceFormatWebP::validate_texture_source(unsupported.path, animated) == ERR_UNAVAILABLE);
	CHECK_FALSE(animated);
	Vector<uint8_t> truncated = TestWebPSafety::make_animation(3);
	REQUIRE(truncated.resize(truncated.size() - 10) == OK);
	TestWebPSafety::Fixture bad(truncated);
	ERR_PRINT_OFF;
	CHECK(ResourceFormatWebP::validate_texture_source(bad.path, animated) == ERR_FILE_CORRUPT);
	ERR_PRINT_ON;
	CHECK_FALSE(animated);
}

TEST_CASE("[E03][WebP] Corrupt final payload is rejected after two valid frames") {
	const Vector<uint8_t> data = corrupt_late_frame();
	TestWebPSafety::Fixture fixture(data);
	uint32_t frames = 0;
	REQUIRE(ResourceFormatWebP::get_webp_frame_count(fixture.path, frames) == OK);
	REQUIRE(frames == 3);
	WebPData encoded = { data.ptr(), size_t(data.size()) };
	WebPAnimDecoder *decoder = WebPAnimDecoderNew(&encoded, nullptr);
	REQUIRE(decoder != nullptr);
	uint8_t *rgba = nullptr;
	int timestamp = 0;
	CHECK(WebPAnimDecoderGetNext(decoder, &rgba, &timestamp));
	CHECK(WebPAnimDecoderGetNext(decoder, &rgba, &timestamp));
	CHECK_FALSE(WebPAnimDecoderGetNext(decoder, &rgba, &timestamp));
	WebPAnimDecoderDelete(decoder);
	bool animated = true;
	CHECK(ResourceFormatWebP::validate_texture_source(fixture.path, animated) == ERR_FILE_CORRUPT);
	CHECK_FALSE(animated);
}

TEST_CASE("[E03][WebP] Actual import validates all frames before copying and retains metadata") {
	ResourceImporterWebPTexture importer;
	for (bool corrupt : { false, true }) {
		TestWebPSafety::Fixture fixture(corrupt ? corrupt_late_frame() : TestWebPSafety::make_animation(64));
		const String destination = fixture.path + ".imported";
		Variant metadata;
		const HashMap<StringName, Variant> options;
		ERR_PRINT_OFF;
		const Error error = importer.import(123, fixture.path, destination, options, nullptr, nullptr, &metadata);
		ERR_PRINT_ON;
		if (corrupt) {
			CHECK(error == ERR_FILE_CORRUPT);
			CHECK_FALSE(FileAccess::exists(destination + ".webp"));
			CHECK(metadata.get_type() == Variant::NIL);
		} else {
			CHECK(error == OK);
			CHECK(FileAccess::get_file_as_bytes(fixture.path) == FileAccess::get_file_as_bytes(destination + ".webp"));
			Dictionary values = metadata;
			CHECK(bool(values["animated_webp"]));
			CHECK(int(values["format_version"]) == importer.get_format_version());
			DirAccess::remove_absolute(destination + ".webp");
		}
	}
}
} // namespace TestWebPImportValidation
#endif
