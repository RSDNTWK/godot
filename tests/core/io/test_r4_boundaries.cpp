/**************************************************************************/
/*  test_r4_boundaries.cpp                                  */
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
TEST_FORCE_LINK(test_r4_boundaries)

#include "core/io/compression.h"
#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "modules/multiplayer/multiplayer_packet_buffer.h"
#include "core/io/file_access_encrypted.h"
#include "core/io/file_access_memory.h"
#include "core/io/marshalls.h"
#include "core/io/resource_format_binary.h"
#include "core/variant/variant_parser.h"
#include "tests/core/io/allocation_test_helpers.h"
#include "tests/test_utils.h"

#include "modules/modules_enabled.gen.h"
#ifdef MODULE_HDR_ENABLED
#include "modules/hdr/image_loader_hdr.h"
#endif
#ifdef MODULE_TGA_ENABLED
#include "modules/tga/image_loader_tga.h"
#endif
#ifdef MODULE_BMP_ENABLED
#include "modules/bmp/image_loader_bmp.h"
#endif
#ifdef MODULE_KTX_ENABLED
#include "modules/ktx/texture_loader_ktx.h"
#endif
#include <cstring>
#ifdef WINDOWS_ENABLED
#include <windows.h>
#endif

namespace TestR4Boundaries {
struct Quiet {
	bool previous = CoreGlobals::print_error_enabled;
	Quiet() { CoreGlobals::print_error_enabled = false; }
	~Quiet() { CoreGlobals::print_error_enabled = previous; }
};
static Ref<FileAccessMemory> memory(const Vector<uint8_t> &p_bytes) {
	Ref<FileAccessMemory> file;
	file.instantiate();
	REQUIRE(file->open_custom(p_bytes.ptr(), p_bytes.size()) == OK);
	return file;
}
static Vector<uint8_t> bytes(int p_size) {
	Vector<uint8_t> result;
	REQUIRE(result.resize(p_size) == OK);
	for (int i = 0; i < p_size; i++) {
		result.write[i] = i % 251;
	}
	return result;
}
TEST_CASE("[R4][Dynamic] Exact limits, trailers, truncation and reused destinations") {
	Quiet quiet;
	const Vector<uint8_t> plain = bytes(65537);
	for (Compression::Mode mode : { Compression::MODE_DEFLATE, Compression::MODE_GZIP }) {
		Vector<uint8_t> compressed;
		REQUIRE(compressed.resize(Compression::get_max_compressed_buffer_size(plain.size(), mode)) == OK);
		const int size = Compression::compress(compressed.ptrw(), plain.ptr(), plain.size(), mode);
		REQUIRE(size > 0);
		REQUIRE(compressed.resize(size) == OK);
		for (int64_t maximum : { int64_t(-1), int64_t(65537), int64_t(65538) }) {
			Vector<uint8_t> output = bytes(17);
			CHECK(Compression::decompress_dynamic(&output, maximum, compressed.ptr(), size, mode) == 0);
			CHECK(output == plain);
		}
		for (int64_t maximum : { int64_t(0), int64_t(65536) }) {
			Vector<uint8_t> output = bytes(17);
			CHECK(Compression::decompress_dynamic(&output, maximum, compressed.ptr(), size, mode) != 0);
			CHECK(output.is_empty());
		}
		for (int missing : { 1, 4, size / 2 }) {
			Vector<uint8_t> output = bytes(17);
			CHECK(Compression::decompress_dynamic(&output, -1, compressed.ptr(), size - missing, mode) != 0);
			CHECK(output.is_empty());
		}
		compressed.push_back(0xb6);
		Vector<uint8_t> output;
		CHECK(Compression::decompress_dynamic(&output, -1, compressed.ptr(), compressed.size(), mode) == 0);
		CHECK(output == plain);
	}
}
#ifdef BROTLI_ENABLED
TEST_CASE("[R4][Dynamic] Brotli exact cap and truncated input") {
	// Brotli stream for the five literal bytes 'hello'.
	const uint8_t encoded[] = { 0x0b, 0x02, 0x80, 'h', 'e', 'l', 'l', 'o', 0x03 };
	Vector<uint8_t> output;
	CHECK(Compression::decompress_dynamic(&output, 5, encoded, sizeof(encoded), Compression::MODE_BROTLI) == 0);
	REQUIRE(output.size() == 5);
	CHECK(memcmp(output.ptr(), "hello", 5) == 0);
	Quiet quiet;
	CHECK(Compression::decompress_dynamic(&output, 4, encoded, sizeof(encoded), Compression::MODE_BROTLI) != 0);
	CHECK(output.is_empty());
	CHECK(Compression::decompress_dynamic(&output, -1, encoded, sizeof(encoded) - 1, Compression::MODE_BROTLI) != 0);
	CHECK(output.is_empty());
}
#endif
class Writer : public FileAccessMemory {
public:
	Vector<uint8_t> written;
	bool opened = true;
	int fault = 0;
	int stores = 0;
	int closes = 0;
	Error error = OK;
	bool is_open() const override { return opened; }
	Error get_error() const override { return error; }
	bool store_buffer(const uint8_t *p_source, uint64_t p_length) override {
		stores++;
		if (fault == 1 || (fault == 2 && stores == 2)) {
			if (fault == 2) {
				const int64_t previous = written.size();
				if (written.resize(previous + p_length / 2) == OK && p_length / 2) {
					memcpy(written.ptrw() + previous, p_source, p_length / 2);
				}
				error = ERR_FILE_CANT_WRITE;
			}
			return false;
		}
		const int64_t previous = written.size();
		if (written.resize(previous + p_length) != OK) {
			return false;
		}
		if (p_length) {
			memcpy(written.ptrw() + previous, p_source, p_length);
		}
		return true;
	}
	void flush() override {
		if (fault == 3) {
			error = ERR_FILE_CANT_WRITE;
		}
	}
	void close() override {
		closes++;
		opened = false;
		if (fault == 4) {
			error = ERR_FILE_CANT_WRITE;
		}
	}
};
TEST_CASE("[R4][Encrypted] Backing store, flush and owned-close failures survive close") {
	const Vector<uint8_t> key = bytes(32);
	const Vector<uint8_t> iv = bytes(16);
	const Vector<uint8_t> plain = bytes(17);
	for (int fault = 0; fault <= 4; fault++) {
		for (bool owned : { false, true }) {
			Ref<Writer> backing;
			backing.instantiate();
			backing->fault = fault;
			FileAccessEncrypted writer;
			REQUIRE(writer.open_and_parse(backing, key, FileAccessEncrypted::MODE_WRITE_AES256, true, iv, owned) == OK);
			REQUIRE(writer.store_buffer(plain.ptr(), plain.size()));
			writer.close();
			const Error expected = fault == 0 || (fault == 4 && !owned) ? OK : ERR_FILE_CANT_WRITE;
			CHECK(writer.get_error() == expected);
			CHECK_FALSE(writer.is_open());
			CHECK(backing->is_open() == !owned);
			CHECK(backing->closes == (owned ? 1 : 0));
			const int stores = backing->stores;
			writer.seek(0);
			writer.close();
			CHECK(writer.get_error() == expected);
			CHECK(backing->stores == stores);
			if (fault == 2) {
				CHECK(backing->written.size() == 60); // Complete 44-byte header plus half the ciphertext.
				FileAccessEncrypted reader;
				Quiet quiet;
				CHECK(reader.open_and_parse(memory(backing->written), key, FileAccessEncrypted::MODE_READ) == ERR_FILE_CORRUPT);
			}
			if (expected == OK) {
				FileAccessEncrypted reader;
				REQUIRE(reader.open_and_parse(memory(backing->written), key, FileAccessEncrypted::MODE_READ) == OK);
				CHECK(reader.FileAccess::get_buffer(plain.size()) == plain);
			}
		}
	}
}
TEST_CASE("[R4][Marshalls] Each output boundary rejects undersized buffers without touching guards") {
	Array array;
	array.push_back(String("a reasonably sized string"));
	array.push_back(bytes(37));
	Dictionary dict;
	dict[String("key")] = array;
	for (const Variant &value : { Variant(), Variant(true), Variant(INT64_MAX), Variant(1.25), Variant(Vector3(1, 2, 3)), Variant(AABB(Vector3(1, 2, 3), Vector3(4, 5, 6))), Variant(String("hello")), Variant(NodePath("one/two:three")), Variant(bytes(13)), Variant(array), Variant(dict) }) {
		int length = 0;
		REQUIRE(encode_variant(value, nullptr, length) == OK);
		for (int capacity = 0; capacity <= length; capacity++) {
			Vector<uint8_t> output;
			REQUIRE(output.resize(length + 16) == OK);
			memset(output.ptrw(), 0xb6, output.size());
			int written = 0;
			Quiet quiet;
			const Error error = encode_variant(value, output.ptrw() + 8, written, false, 0, capacity);
			CHECK((error == OK) == (capacity == length));
			for (int i = 0; i < 8; i++) {
				CHECK(output[i] == 0xb6);
			}
			for (int i = 8 + capacity; i < output.size(); i++) {
				CHECK(output[i] == 0xb6);
			}
			if (error == OK) {
				Variant decoded;
				REQUIRE(decode_variant(decoded, output.ptr() + 8, written) == OK);
				CHECK(decoded == value);
			}
		}
	}
}
TEST_CASE("[R4][Text] Deep containers fail and a reused stream has a clean recursion state") {
	VariantParser::StreamString stream;
	stream.s = String("[").repeat(Variant::MAX_RECURSION_DEPTH + 8) + "0" + String("]").repeat(Variant::MAX_RECURSION_DEPTH + 8);
	Variant output;
	String message;
	int line = 0;
	Quiet quiet;
	CHECK(VariantParser::parse(&stream, output, message, line) != OK);
	CHECK(stream.parse_depth == 0);
	VariantParser::StreamString control;
	control.s = "{\"items\": [1, 2, PackedByteArray(3, 4)]}";
	CHECK(VariantParser::parse(&control, output, message, line) == OK);
	CHECK(control.parse_depth == 0);
}
#ifdef B01_ALLOCATION_TESTS
template <typename F>
static void image_allocation_failure(F p_load, const Ref<Image> &p_image, int p_bytes, int p_occurrence = 1, Error p_expected_error = ERR_OUT_OF_MEMORY) {
	const Vector<uint8_t> before = p_image->get_data();
	const size_t allocation = TestAllocation::allocation_size([&]() { Vector<uint8_t> probe; REQUIRE(probe.resize(p_bytes) == OK); });
	Error error;
	int failures;
	{
		Quiet quiet;
		TestAllocation::Failure fault(allocation, p_occurrence);
		error = p_load();
		failures = TestAllocation::failures();
	}
	CHECK(failures == 1);
	CHECK(error == p_expected_error);
	CHECK(p_image->get_data() == before);
}
#endif
#ifdef MODULE_HDR_ENABLED
static Vector<uint8_t> hdr(const char *p_dimensions, const Vector<uint8_t> &p_payload) {
	Vector<uint8_t> result = String("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n" + String(p_dimensions) + "\n").to_utf8_buffer();
	result.append_array(p_payload);
	return result;
}
TEST_CASE("[R4][HDR] RLE packet extent and progress checks preserve the destination") {
	ImageLoaderHDR loader;
	Vector<uint8_t> payload;
	for (uint8_t byte : { 2, 2, 0, 8, 136, 10, 136, 20, 136, 30, 136, 128 }) {
		payload.push_back(byte);
	}
	const Vector<uint8_t> valid = hdr("-Y 1 +X 8", payload);
	Ref<Image> image;
	image.instantiate();
	REQUIRE(loader.load_image(image, memory(valid), 0, 1.0) == OK);
	CHECK(image->get_size() == Vector2i(8, 1));
#ifdef B01_ALLOCATION_TESTS
	Ref<FileAccess> source = memory(valid);
	image_allocation_failure([&]() { return loader.load_image(image, source, 0, 1.0); }, image, 32);
#endif
	for (int packet : { 0, 9, 137, 255 }) {
		Vector<uint8_t> damaged = valid;
		damaged.write[valid.size() - payload.size() + 4] = packet;
		Ref<Image> destination = Image::create_empty(2, 2, false, Image::FORMAT_RGBA8);
		const Vector<uint8_t> before = destination->get_data();
		Quiet quiet;
		CHECK(loader.load_image(destination, memory(damaged), 0, 1.0) != OK);
		CHECK(destination->get_data() == before);
	}
	for (const char *dimensions : { "-Y 0 +X 8", "-Y 1 +X 4294967304", "-Y 268435456 +X 8" }) {
		const Vector<uint8_t> damaged = hdr(dimensions, payload);
		Quiet quiet;
		CHECK(loader.load_image(image, memory(damaged), 0, 1.0) != OK);
	}
	Vector<uint8_t> truncated = valid;
	REQUIRE(truncated.resize(truncated.size() - 1) == OK);
	Quiet quiet;
	CHECK(loader.load_image(image, memory(truncated), 0, 1.0) != OK);
}
#endif
#ifdef MODULE_TGA_ENABLED
TEST_CASE("[R4][TGA] Type, palette origin and truncated initialized extent") {
	ImageLoaderTGA loader;
	Vector<uint8_t> fixture;
	REQUIRE(fixture.resize(22) == OK);
	memset(fixture.ptrw(), 0, fixture.size());
	fixture.write[1] = 1;
	fixture.write[2] = 1;
	fixture.write[3] = 7;
	fixture.write[5] = 1;
	fixture.write[7] = 24;
	fixture.write[12] = 1;
	fixture.write[14] = 1;
	fixture.write[16] = 8;
	fixture.write[18] = 30;
	fixture.write[19] = 20;
	fixture.write[20] = 10;
	fixture.write[21] = 7;
	Ref<Image> image;
	image.instantiate();
	REQUIRE(loader.load_image(image, memory(fixture), 0, 1.0) == OK);
	CHECK(image->get_size() == Vector2i(1, 1));
	CHECK(image->get_data()[0] == 10);
#ifdef B01_ALLOCATION_TESTS
	for (int allocation_bytes : { 3, 1, 4 }) {
		Ref<FileAccess> source = memory(fixture);
		image_allocation_failure([&]() { return loader.load_image(image, source, 0, 1.0); }, image, allocation_bytes);
	}
#endif
	for (int kind = 0; kind < 4; kind++) {
		Vector<uint8_t> damaged = fixture;
		if (kind == 0) {
			damaged.write[16] = 16;
		}
		if (kind == 1) {
			damaged.write[21] = 6;
		}
		if (kind == 2) {
			REQUIRE(damaged.resize(21) == OK);
		}
		if (kind == 3) {
			REQUIRE(damaged.resize(17) == OK);
		}
		Quiet quiet;
		CHECK(loader.load_image(image, memory(damaged), 0, 1.0) != OK);
	}
}
#endif

static void append_u32(Vector<uint8_t> &r_bytes, uint32_t p_value) {
	const int previous = r_bytes.size();
	REQUIRE(r_bytes.resize(previous + 4) == OK);
	encode_uint32(p_value, r_bytes.ptrw() + previous);
}
static void append_u64(Vector<uint8_t> &r_bytes, uint64_t p_value) {
	const int previous = r_bytes.size();
	REQUIRE(r_bytes.resize(previous + 8) == OK);
	encode_uint64(p_value, r_bytes.ptrw() + previous);
}
static void append_string(Vector<uint8_t> &r_bytes, const String &p_value) {
	const CharString utf8 = p_value.utf8();
	append_u32(r_bytes, utf8.length() + 1);
	for (int i = 0; i <= utf8.length(); i++) {
		REQUIRE_FALSE(r_bytes.push_back(utf8[i]));
	}
}
static Vector<uint8_t> resource_bytes(const Vector<uint8_t> &p_payload) {
	Vector<uint8_t> result = String("RSRC").to_utf8_buffer();
	append_u32(result, 0); // Little endian.
	append_u32(result, 0); // Wire real width.
	append_u32(result, 4);
	append_u32(result, 7);
	append_u32(result, 6);
	append_string(result, "Resource");
	append_u64(result, 0); // Metadata offset.
	append_u32(result, 0); // Flags.
	append_u64(result, 0); // UID.
	for (int i = 0; i < 11; i++) {
		append_u32(result, 0);
	}
	append_u32(result, 1);
	append_string(result, "metadata/r4");
	append_u32(result, 0); // External resources.
	append_u32(result, 1); // Internal resources.
	append_string(result, "");
	append_u64(result, result.size() + 8);
	append_string(result, "Resource");
	append_u32(result, 1); // One property.
	append_u32(result, 0); // String table index.
	result.append_array(p_payload);
	return result;
}
static Ref<Resource> load_resource_bytes(const Vector<uint8_t> &p_bytes, Error &r_error) {
	const String path = TestUtils::get_temp_path("r4_binary_fixture.res");
	{
		Ref<FileAccess> output = FileAccess::open(path, FileAccess::WRITE);
		REQUIRE(output.is_valid());
		if (output.is_null()) { r_error = FileAccess::get_open_error(); return Ref<Resource>(); }
		REQUIRE(output->store_buffer(p_bytes));
	}
	ResourceFormatLoaderBinary loader;
	return loader.load(path, path, &r_error, false, nullptr, ResourceFormatLoader::CACHE_MODE_IGNORE);
}
TEST_CASE("[R4][BinaryResource] Packed payload truncation fails before publishing a value") {
	PackedInt32Array ints;
	ints.push_back(123);
	PackedInt64Array longs;
	longs.push_back(INT64_C(12345678901));
	PackedFloat32Array floats;
	floats.push_back(1.25f);
	PackedFloat64Array doubles;
	doubles.push_back(1.25);
	PackedVector2Array vectors2;
	vectors2.push_back(Vector2(1, 2));
	PackedVector3Array vectors3;
	vectors3.push_back(Vector3(1, 2, 3));
	PackedVector4Array vectors4;
	vectors4.push_back(Vector4(1, 2, 3, 4));
	PackedColorArray colors;
	colors.push_back(Color(0.25, 0.5, 0.75, 1));
	for (const Variant &value : { Variant(bytes(5)), Variant(ints), Variant(longs), Variant(floats), Variant(doubles), Variant(vectors2), Variant(vectors3), Variant(vectors4), Variant(colors) }) {
		Ref<Writer> output;
		output.instantiate();
		HashMap<Ref<Resource>, int> resources;
		HashMap<Ref<Resource>, int> external;
		HashMap<StringName, int> strings;
		REQUIRE(ResourceFormatSaverBinaryInstance::write_variant(output, value, resources, external, strings) == OK);
		const Vector<uint8_t> fixture = resource_bytes(output->written);
		Error control_error;
		const Ref<Resource> control = load_resource_bytes(fixture, control_error);
		REQUIRE(control_error == OK);
		REQUIRE(control.is_valid());
		if (control.is_null()) { continue; }
		CHECK(control->get_meta("r4") == value);
		for (int missing = 1; missing <= MIN(8, output->written.size() - 8); missing++) {
			Vector<uint8_t> damaged = fixture;
			REQUIRE(damaged.resize(damaged.size() - missing) == OK);
			Quiet quiet;
			Error error;
			CHECK(load_resource_bytes(damaged, error).is_null());
			CHECK(error != OK);
		}
	}
}
TEST_CASE("[R4][BinaryResource] Oversized string prefixes and excessive nesting are errors") {
	Vector<uint8_t> nil;
	append_u32(nil, 1);
	for (uint32_t length : { uint32_t(INT_MAX), uint32_t(INT_MAX) + 1, UINT32_MAX }) {
		Vector<uint8_t> damaged = resource_bytes(nil);
		encode_uint32(length, damaged.ptrw() + 24); // Resource type string.
		ResourceLoaderBinary loader;
		Quiet quiet;
		loader.open(memory(damaged));
		CHECK(loader.load() != OK);
		CHECK(loader.get_resource().is_null());
		ResourceLoaderBinary recognizer;
		CHECK(recognizer.recognize(memory(damaged)).is_empty());
	}
	Vector<uint8_t> nested;
	for (int i = 0; i < Variant::MAX_RECURSION_DEPTH + 8; i++) {
		append_u32(nested, 30); // Array.
		append_u32(nested, 1);
	}
	append_u32(nested, 1); // NIL.
	const Vector<uint8_t> fixture = resource_bytes(nested);
	Quiet quiet;
	Error error;
	CHECK(load_resource_bytes(fixture, error).is_null());
	CHECK(error != OK);
}
TEST_CASE("[R4][BinarySaver] Cyclic discovery fails before destination truncation") {
	const String path = TestUtils::get_temp_path("r4_cycle.res");
	const Vector<uint8_t> before = bytes(17);
	{
		Ref<FileAccess> output = FileAccess::open(path, FileAccess::WRITE);
		REQUIRE(output.is_valid());
		REQUIRE(output->store_buffer(before));
	}
	Ref<Resource> resource;
	resource.instantiate();
	Array cycle;
	cycle.push_back(cycle);
	resource->set_meta("r4", cycle);
	ResourceFormatSaverBinaryInstance saver;
	{
		Quiet quiet;
		CHECK(saver.save(path, resource) != OK);
	}
	CHECK(FileAccess::get_file_as_bytes(path) == before);
	cycle.clear();
}

#ifdef B01_ALLOCATION_TESTS
TEST_CASE("[R4][Allocation] Dynamic output growth and final compaction propagate failure") {
	const Vector<uint8_t> plain = bytes(65537);
	Vector<uint8_t> compressed;
	REQUIRE(compressed.resize(Compression::get_max_compressed_buffer_size(plain.size(), Compression::MODE_GZIP)) == OK);
	const int size = Compression::compress(compressed.ptrw(), plain.ptr(), plain.size(), Compression::MODE_GZIP);
	REQUIRE(size > 0);
	struct GrowthFault { int destination; int occurrence; };
	for (const GrowthFault &growth : { GrowthFault{ Compression::gzip_chunk, 1 }, GrowthFault{ Compression::gzip_chunk * 2, 1 }, GrowthFault{ int(plain.size()), 1 }, GrowthFault{ Compression::gzip_chunk * 3, 1 } }) {
		const size_t allocation = TestAllocation::allocation_size([&]() {
			Vector<uint8_t> probe;
			REQUIRE(probe.resize(growth.destination) == OK);
		});
		Vector<uint8_t> output;
		int result;
		int failures;
		{
			Quiet quiet;
			TestAllocation::Failure fault(allocation, growth.occurrence);
			result = Compression::decompress_dynamic(&output, -1, compressed.ptr(), size, Compression::MODE_GZIP);
			failures = TestAllocation::failures();
		}
		CHECK(failures == 1);
		CHECK(result == Z_MEM_ERROR);
		CHECK(output.is_empty());
	}
}
TEST_CASE("[R4][Allocation] Packed binary and text readers report destination allocation failure") {
	PackedFloat64Array values;
	REQUIRE(values.resize(127) == OK);
	Variant source = values;
	int length = 0;
	REQUIRE(encode_variant(source, nullptr, length) == OK);
	Vector<uint8_t> encoded;
	REQUIRE(encoded.resize(length) == OK);
	REQUIRE(encode_variant(source, encoded.ptrw(), length, false, 0, length) == OK);
	const size_t allocation = TestAllocation::allocation_size([]() {
		Vector<double> probe;
		REQUIRE(probe.resize(127) == OK);
	});
	Variant result;
	Error error;
	int failures;
	{
		Quiet quiet;
		TestAllocation::Failure fault(allocation);
		error = decode_variant(result, encoded.ptr(), encoded.size());
		failures = TestAllocation::failures();
	}
	CHECK(failures == 1);
	CHECK(error == ERR_OUT_OF_MEMORY);
	CHECK(result.get_type() == Variant::NIL);
	const size_t text_allocation = TestAllocation::allocation_size([]() {
		Vector<double> probe;
		REQUIRE(probe.resize(1) == OK);
	});
	VariantParser::StreamString stream;
	stream.s = "PackedFloat64Array(12, 34)";
	String message;
	int line = 0;
	{
		Quiet quiet;
		TestAllocation::Failure fault(text_allocation);
		error = VariantParser::parse(&stream, result, message, line);
		failures = TestAllocation::failures();
	}
	CHECK(failures == 1);
	CHECK(error == ERR_OUT_OF_MEMORY);
	CHECK(stream.parse_depth == 0);
}
#endif

#ifdef MODULE_BMP_ENABLED
class ShortImageReader : public FileAccessMemory {
public:
	int fail_at = -1;
	bool hard = false;
	bool short_read = true;
	mutable int calls = 0;
	mutable Error error = OK;
	uint64_t get_buffer(uint8_t *p_buffer, uint64_t p_length) const override {
		if (calls++ == fail_at) {
			if (hard) { error = ERR_FILE_CANT_READ; }
			if (short_read && p_length) { p_length--; }
		}
		return FileAccessMemory::get_buffer(p_buffer, p_length);
	}
	Error get_error() const override { return error != OK ? error : FileAccessMemory::get_error(); }
};
TEST_CASE("[R4][BMP] Header, pixel short reads and invalid signatures never report success") {
	Vector<uint8_t> fixture;
	REQUIRE(fixture.resize(58) == OK);
	memset(fixture.ptrw(), 0, fixture.size());
	uint8_t *data = fixture.ptrw();
	data[0] = 'B'; data[1] = 'M';
	encode_uint32(58, data + 2); encode_uint32(54, data + 10);
	encode_uint32(40, data + 14); encode_uint32(1, data + 18); encode_uint32(1, data + 22);
	encode_uint16(1, data + 26); encode_uint16(24, data + 28); encode_uint32(4, data + 34);
	data[54] = 30; data[55] = 20; data[56] = 10;
	ImageLoaderBMP loader;
	Ref<Image> control;
	control.instantiate();
	REQUIRE(loader.load_image(control, memory(fixture), 0, 1.0) == OK);
	CHECK(control->get_size() == Vector2i(1, 1));
	CHECK(control->get_data()[0] == 10);
#ifdef B01_ALLOCATION_TESTS
	for (int occurrence : { 1, 2 }) {
		Ref<FileAccess> source = memory(fixture);
		image_allocation_failure([&]() { return loader.load_image(control, source, 0, 1.0); }, control, 4, occurrence);
	}
#endif
	const Vector<uint8_t> before = control->get_data();
	for (int call : { 0, 1 }) {
		for (bool hard : { false, true }) {
			Ref<ShortImageReader> input;
			input.instantiate();
			REQUIRE(input->open_custom(fixture.ptr(), fixture.size()) == OK);
			input->fail_at = call; input->hard = hard;
			Quiet quiet;
			CHECK(loader.load_image(control, input, 0, 1.0) != OK);
			CHECK(control->get_data() == before);
		}
	}
	fixture.write[0] = 'X';
	Quiet quiet;
	CHECK(loader.load_image(control, memory(fixture), 0, 1.0) == ERR_INVALID_DATA);
	CHECK(control->get_data() == before);
	CHECK(control->load_bmp_from_buffer(fixture) != OK);
	CHECK(control->get_data() == before);
}
#endif
#ifdef MODULE_KTX_ENABLED
static Vector<uint8_t> ktx1(bool p_integer = false) {
	const uint8_t identifier[] = { 0xab, 'K', 'T', 'X', ' ', '1', '1', 0xbb, 13, 10, 26, 10 };
	Vector<uint8_t> result;
	REQUIRE(result.resize(p_integer ? 84 : 72) == OK);
	memset(result.ptrw(), 0, result.size());
	memcpy(result.ptrw(), identifier, 12);
	uint8_t *out = result.ptrw();
	encode_uint32(0x04030201, out + 12);
	encode_uint32(p_integer ? 0x1404 : 0x1401, out + 16);
	encode_uint32(p_integer ? 4 : 1, out + 20);
	encode_uint32(p_integer ? 0x8d99 : 0x1908, out + 24);
	encode_uint32(p_integer ? 0x8d82 : 0x8058, out + 28);
	encode_uint32(0x1908, out + 32);
	encode_uint32(1, out + 36); encode_uint32(1, out + 40);
	encode_uint32(1, out + 52); encode_uint32(1, out + 56);
	encode_uint32(p_integer ? 16 : 4, out + 64);
	out[68] = 10; out[69] = 20; out[70] = 30; out[71] = 255;
	return result;
}
TEST_CASE("[R4][KTX] Valid memory loads and rejected inputs preserve image and error contracts") {
	ResourceFormatKTX loader;
	Ref<Image> image;
	image.instantiate();
	const Vector<uint8_t> valid = ktx1();
	REQUIRE(image->load_ktx_from_buffer(valid) == OK);
	CHECK(image->get_size() == Vector2i(1, 1));
#ifdef B01_ALLOCATION_TESTS
	image_allocation_failure([&]() { return image->load_ktx_from_buffer(valid); }, image, 4, 1, ERR_PARSE_ERROR);
#endif
	const Vector<uint8_t> before = image->get_data();
	for (int kind = 0; kind < 3; kind++) {
		Vector<uint8_t> invalid = kind == 0 ? ktx1(true) : valid;
		if (kind == 1) { REQUIRE(invalid.resize(invalid.size() - 1) == OK); }
		if (kind == 2) { invalid.write[0] = 0; }
		Quiet quiet;
		CHECK(image->load_ktx_from_buffer(invalid) != OK);
		CHECK(image->get_data() == before);
	}
}
#endif
TEST_CASE("[R4][PacketBuffer] Frame sizes are representable before allocation") {
	Vector<uint8_t> buffer = bytes(8);
	const Vector<uint8_t> before = buffer;
	CHECK(MultiplayerPacketBuffer::ensure_size(buffer, int64_t(INT_MAX) + 1) == ERR_OUT_OF_MEMORY);
	CHECK(MultiplayerPacketBuffer::ensure_size(buffer, -1) == ERR_OUT_OF_MEMORY);
	CHECK(buffer == before);
	CHECK(MultiplayerPacketBuffer::ensure_size(buffer, 8) == OK);
	CHECK(buffer == before);
}
#ifdef B01_ALLOCATION_TESTS
TEST_CASE("[R4][PacketBuffer][Allocation] Failed frame growth retains the smaller cache and returns failure") {
	Vector<uint8_t> buffer = bytes(8);
	const Vector<uint8_t> before = buffer;
	const size_t allocation = TestAllocation::allocation_size([]() { Vector<uint8_t> probe; REQUIRE(probe.resize(4096) == OK); });
	Error error;
	int failures;
	{
		Quiet quiet;
		TestAllocation::Failure fault(allocation);
		error = MultiplayerPacketBuffer::ensure_size(buffer, 4096);
		failures = TestAllocation::failures();
	}
	CHECK(failures == 1);
	CHECK(error == ERR_OUT_OF_MEMORY);
	CHECK(buffer == before);
}
#endif
#ifdef WINDOWS_ENABLED
TEST_CASE("[R4][Encrypted][SafeSave] Failed destination replacement reaches ConfigFile callers") {
	const String path = TestUtils::get_temp_path("r4_locked_encrypted.cfg");
	const Vector<uint8_t> original = bytes(17);
	const bool previous = FileAccess::is_backup_save_enabled();
	FileAccess::set_backup_save(false);
	{
		Ref<FileAccess> output = FileAccess::open(path, FileAccess::WRITE);
		REQUIRE(output.is_valid());
		if (output.is_null()) { FileAccess::set_backup_save(previous); return; }
		REQUIRE(output->store_buffer(original));
	}
	const Char16String filename = path.utf16();
	HANDLE locked = CreateFileW(reinterpret_cast<LPCWSTR>(filename.ptr()), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	REQUIRE(locked != INVALID_HANDLE_VALUE);
	if (locked == INVALID_HANDLE_VALUE) { FileAccess::set_backup_save(previous); return; }
	FileAccess::set_backup_save(true);
	ConfigFile config;
	config.set_value("test", "value", 42);
	{
		Quiet quiet;
		CHECK(config.save_encrypted(path, bytes(32)) == ERR_FILE_CANT_WRITE);
		CHECK(config.save_encrypted_pass(path, "ordinary-test-password") == ERR_FILE_CANT_WRITE);
	}
	CloseHandle(locked);
	FileAccess::set_backup_save(previous);
	CHECK(FileAccess::get_file_as_bytes(path) == original);
}
#endif

#ifdef B01_ALLOCATION_TESTS
TEST_CASE("[R4][Allocation] Decompression provider initialization failures leave no output") {
	Vector<uint8_t> plain = bytes(17);
	Vector<uint8_t> gzip;
	REQUIRE(gzip.resize(Compression::get_max_compressed_buffer_size(plain.size(), Compression::MODE_GZIP)) == OK);
	const int length = Compression::compress(gzip.ptrw(), plain.ptr(), plain.size(), Compression::MODE_GZIP);
	REQUIRE(length > 0);
	const size_t provider_size = TestAllocation::allocation_size([&]() {
		Vector<uint8_t> result;
		REQUIRE(Compression::decompress_dynamic(&result, -1, gzip.ptr(), length, Compression::MODE_GZIP) == Z_OK);
	});
	REQUIRE(provider_size > 0);
	Vector<uint8_t> result = bytes(7);
	int error;
	int failures;
	{
		Quiet quiet;
		TestAllocation::Failure fault(provider_size);
		error = Compression::decompress_dynamic(&result, -1, gzip.ptr(), length, Compression::MODE_GZIP);
		failures = TestAllocation::failures();
	}
	CHECK(failures == 1);
	CHECK(error == Z_MEM_ERROR);
	CHECK(result.is_empty());
#ifdef BROTLI_ENABLED
	const uint8_t brotli[] = { 0x0b, 0x02, 0x80, 'h', 'e', 'l', 'l', 'o', 0x03 };
	const size_t brotli_size = TestAllocation::allocation_size([&]() {
		Vector<uint8_t> output;
		REQUIRE(Compression::decompress_dynamic(&output, -1, brotli, sizeof(brotli), Compression::MODE_BROTLI) == Z_OK);
	});
	REQUIRE(brotli_size > 0);
	{
		Quiet quiet;
		TestAllocation::Failure fault(brotli_size);
		error = Compression::decompress_dynamic(&result, -1, brotli, sizeof(brotli), Compression::MODE_BROTLI);
		failures = TestAllocation::failures();
	}
	CHECK(failures == 1);
	CHECK(error == Z_MEM_ERROR);
	CHECK(result.is_empty());
#endif
}
TEST_CASE("[R4][Encrypted][Allocation] Preparation failures publish no encrypted header") {
	const Vector<uint8_t> key = bytes(32);
	const Vector<uint8_t> iv = bytes(16);
	const Vector<uint8_t> plain = bytes(17);
	const size_t staging_size = TestAllocation::allocation_size([]() { Vector<uint8_t> probe; REQUIRE(probe.resize(32) == OK); });
	CryptoCore::AESContext probe;
	REQUIRE(probe.set_encode_key(key.ptr(), 256) == OK);
	uint8_t probe_iv[16] = {};
	uint8_t block[32] = {};
	const size_t cipher_size = TestAllocation::allocation_size([&]() { REQUIRE(probe.encrypt_cfb(sizeof(block), probe_iv, block, block) == OK); });
	REQUIRE(cipher_size > 0);
	for (size_t allocation : { staging_size, cipher_size }) {
		Ref<Writer> backing;
		backing.instantiate();
		FileAccessEncrypted writer;
		REQUIRE(writer.open_and_parse(backing, key, FileAccessEncrypted::MODE_WRITE_AES256, true, iv) == OK);
		REQUIRE(writer.store_buffer(plain.ptr(), plain.size()));
		int failures;
		{
			Quiet quiet;
			TestAllocation::Failure fault(allocation);
			writer.close();
			failures = TestAllocation::failures();
		}
		CHECK(failures == 1);
		CHECK(writer.get_error() != OK);
		CHECK(backing->stores == 0);
		CHECK(backing->written.is_empty());
	}
}
#endif
} // namespace TestR4Boundaries
