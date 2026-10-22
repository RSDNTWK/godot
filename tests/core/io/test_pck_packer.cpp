/**************************************************************************/
/*  test_pck_packer.cpp                                                   */
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

TEST_FORCE_LINK(test_pck_packer)

#include "core/io/file_access.h"
#include "core/io/dir_access.h"
#include "core/io/pck_lzma.h"
#include "core/io/pck_packer.h"
#include "core/os/os.h"
#include "tests/test_utils.h"

#ifdef TOOLS_ENABLED
#include "editor/export/pck_compression_cache.h"
#endif

namespace TestPCKPacker {

TEST_CASE("[PCKLzma] Tiny independent entries use a small decoder dictionary") {
	for (int threads : { 1, 16 }) {
		PCKLzmaOptions options;
		options.threads = threads;
		for (int size : { 1, 31, 4096 }) {
			Vector<uint8_t> input;
			input.resize(size);
			for (int i = 0; i < size; i++) {
				input.write[i] = uint8_t(i * 37);
			}
			Vector<uint8_t> compressed;
			REQUIRE(compress_lzma2(input, compressed, options) == OK);
			REQUIRE(compressed.size() > 1);
			CHECK_MESSAGE(compressed[0] == 0, "A payload of at most 4 KiB must not request a 256 MiB decoder dictionary.");
			Vector<uint8_t> decoded;
			REQUIRE(decompress_lzma2(compressed.ptr(), compressed.size(), input.size(), decoded) == OK);
			CHECK(decoded == input);
		}
	}
}

TEST_CASE("[PCKLzma] Memory input round trips mixed and incompressible entries") {
	for (int threads : { 1, 16 }) {
		PCKLzmaOptions options;
		options.threads = threads;
		for (bool repeating : { false, true }) {
			Vector<uint8_t> input;
			input.resize(1024 * 1024 + 17);
			uint32_t state = 0x8a75c193;
			for (int i = 0; i < input.size(); i++) {
				state ^= state << 13;
				state ^= state >> 17;
				state ^= state << 5;
				input.write[i] = repeating && i >= 65536 ? input[i % 65536] : uint8_t(state);
			}
			Vector<uint8_t> compressed;
			REQUIRE(compress_lzma2(input, compressed, options) == OK);
			Vector<uint8_t> decoded;
			REQUIRE(decompress_lzma2(compressed.ptr(), compressed.size(), input.size(), decoded) == OK);
			CHECK(decoded == input);
			if (repeating) {
				CHECK(compressed.size() < input.size() / 2);
			}
		}
	}
}

TEST_CASE("[PCKLzma] Empty input clears a previous result") {
	Vector<uint8_t> compressed;
	compressed.push_back(123);
	CHECK(compress_lzma2(Vector<uint8_t>(), compressed, PCKLzmaOptions()) == OK);
	CHECK(compressed.is_empty());
}

#ifdef TOOLS_ENABLED
struct CompressionCacheFixture {
	String path = TestUtils::get_temp_path("pck_compression_cache_" + uitos(OS::get_singleton()->get_ticks_usec()));
	~CompressionCacheFixture() {
		Ref<DirAccess> dir = DirAccess::open(path);
		if (dir.is_valid()) {
			for (const String &name : dir->get_files()) {
				DirAccess::remove_absolute(path.path_join(name));
			}
			dir.unref();
			DirAccess::remove_absolute(path);
		}
	}
};

static Vector<uint8_t> cache_input(uint8_t p_value = 42) {
	Vector<uint8_t> input;
	input.resize(1024 * 1024);
	input.fill(p_value);
	return input;
}

TEST_CASE("[PCKLzmaCache] Exact input and every encoder setting invalidate the key") {
	Vector<uint8_t> input = cache_input();
	PCKLzmaOptions options;
	const String key = PCKCompressionCache::make_key(input, options);
	CHECK(key.length() == 64);
	CHECK(PCKCompressionCache::make_key(input, options) == key);
	input.write[0]++;
	CHECK(PCKCompressionCache::make_key(input, options) != key);
	input.write[0]--;
	for (int field = 0; field < 5; field++) {
		PCKLzmaOptions changed = options;
		switch (field) {
			case 0: changed.compression_level = 7; break;
			case 1: changed.dictionary_size_mb = 128; break;
			case 2: changed.word_size = 32; break;
			case 3: changed.threads = 16; break;
			case 4: changed.memory_usage_percent = 70; break;
		}
		CHECK(PCKCompressionCache::make_key(input, changed) != key);
	}
}

TEST_CASE("[PCKLzmaCache] Compressed entries survive reopening and validate exact bytes") {
	CompressionCacheFixture fixture;
	PCKCompressionCache cache(fixture.path);
	REQUIRE(cache.is_available());
	const Vector<uint8_t> input = cache_input();
	PCKLzmaOptions options;
	const String key = PCKCompressionCache::make_key(input, options);
	Vector<uint8_t> output;
	CHECK(cache.load(key, input, output) == PCKCompressionCache::MISS);
	Vector<uint8_t> candidate;
	REQUIRE(compress_lzma2(input, candidate, options) == OK);
	cache.store(key, input, candidate);
	CHECK(cache.load(key, input, output) == PCKCompressionCache::COMPRESSED);
	CHECK(output == candidate);
	PCKCompressionCache reopened(fixture.path);
	CHECK(reopened.load(key, input, output) == PCKCompressionCache::COMPRESSED);
	CHECK(output == candidate);
}

TEST_CASE("[PCKLzmaCache] Truncated and mismatched entries become cache misses") {
	CompressionCacheFixture fixture;
	PCKCompressionCache cache(fixture.path);
	const Vector<uint8_t> input = cache_input();
	PCKLzmaOptions options;
	const String key = PCKCompressionCache::make_key(input, options);
	Vector<uint8_t> different;
	REQUIRE(compress_lzma2(cache_input(43), different, options) == OK);
	cache.store(key, input, different);
	Vector<uint8_t> output;
	CHECK(cache.load(key, input, output) == PCKCompressionCache::MISS);
	CHECK(output.is_empty());
	CHECK_FALSE(FileAccess::exists(fixture.path.path_join(key + ".lzma2")));
	Vector<uint8_t> candidate;
	REQUIRE(compress_lzma2(input, candidate, options) == OK);
	cache.store(key, input, candidate);
	Ref<FileAccess> file = FileAccess::open(fixture.path.path_join(key + ".lzma2"), FileAccess::WRITE);
	REQUIRE(file.is_valid());
	file->store_32(123);
	file.unref();
	CHECK(cache.load(key, input, output) == PCKCompressionCache::MISS);
	CHECK(output.is_empty());
}

TEST_CASE("[PCKLzmaCache] Raw markers avoid ineffective compression without returning empty compressed data") {
	CompressionCacheFixture fixture;
	PCKCompressionCache cache(fixture.path);
	const Vector<uint8_t> input = cache_input();
	const String key = PCKCompressionCache::make_key(input, PCKLzmaOptions());
	cache.store(key, input, input);
	Vector<uint8_t> output;
	output.push_back(123);
	CHECK(cache.load(key, input, output) == PCKCompressionCache::RAW);
	CHECK(output.is_empty());
}

TEST_CASE("[PCKLzmaCache] Disk budget evicts old entries and an unavailable cache is optional") {
	CompressionCacheFixture fixture;
	const Vector<uint8_t> first = cache_input();
	const Vector<uint8_t> second = cache_input(43);
	PCKLzmaOptions options;
	Vector<uint8_t> first_candidate, second_candidate;
	REQUIRE(compress_lzma2(first, first_candidate, options) == OK);
	REQUIRE(compress_lzma2(second, second_candidate, options) == OK);
	const uint64_t budget = 2 * 92 + first_candidate.size() + second_candidate.size() - 1;
	PCKCompressionCache cache(fixture.path, budget);
	const String first_key = PCKCompressionCache::make_key(first, options);
	const String second_key = PCKCompressionCache::make_key(second, options);
	cache.store(first_key, first, first_candidate);
	cache.store(second_key, second, second_candidate);
	Vector<uint8_t> output;
	CHECK(cache.load(first_key, first, output) == PCKCompressionCache::MISS);
	CHECK(cache.load(second_key, second, output) == PCKCompressionCache::COMPRESSED);
	PCKCompressionCache disabled(String(), 0);
	CHECK_FALSE(disabled.is_available());
	disabled.store(first_key, first, first_candidate);
	CHECK(disabled.load(first_key, first, output) == PCKCompressionCache::MISS);
}
#endif // TOOLS_ENABLED

TEST_CASE("[PCKPacker] Pack an empty PCK file") {
	PCKPacker pck_packer;
	const String output_pck_path = TestUtils::get_temp_path("output_empty.pck");
	CHECK_MESSAGE(
			pck_packer.pck_start(output_pck_path) == OK,
			"Starting a PCK file should return an OK error code.");

	CHECK_MESSAGE(
			pck_packer.flush() == OK,
			"Flushing the PCK should return an OK error code.");

	Error err;
	Ref<FileAccess> f = FileAccess::open(output_pck_path, FileAccess::READ, &err);
	CHECK_MESSAGE(
			err == OK,
			"The generated empty PCK file should be opened successfully.");
	CHECK_MESSAGE(
			f->get_length() >= 100,
			"The generated empty PCK file shouldn't be too small (it should have the PCK header).");
	CHECK_MESSAGE(
			f->get_length() <= 500,
			"The generated empty PCK file shouldn't be too large.");
}

TEST_CASE("[PCKPacker] Pack empty with zero alignment invalid") {
	PCKPacker pck_packer;
	const String output_pck_path = TestUtils::get_temp_path("output_empty.pck");
	ERR_PRINT_OFF;
	CHECK_MESSAGE(pck_packer.pck_start(output_pck_path, 0) != OK, "PCK with zero alignment should fail.");
	ERR_PRINT_ON;
}

TEST_CASE("[PCKPacker] Pack empty with invalid key") {
	PCKPacker pck_packer;
	const String output_pck_path = TestUtils::get_temp_path("output_empty.pck");
	ERR_PRINT_OFF;
	CHECK_MESSAGE(pck_packer.pck_start(output_pck_path, 32, "") != OK, "PCK with invalid key should fail.");
	ERR_PRINT_ON;
}

TEST_CASE("[PCKPacker] Pack a PCK file with some files and directories") {
	PCKPacker pck_packer;
	const String output_pck_path = TestUtils::get_temp_path("output_with_files.pck");
	CHECK_MESSAGE(
			pck_packer.pck_start(output_pck_path) == OK,
			"Starting a PCK file should return an OK error code.");

	const String base_dir = OS::get_singleton()->get_executable_path().get_base_dir();

	CHECK_MESSAGE(
			pck_packer.add_file("version.py", base_dir.path_join("../version.py"), "version.py") == OK,
			"Adding a file to the PCK should return an OK error code.");
	CHECK_MESSAGE(
			pck_packer.add_file("some/directories with spaces/to/create/icon.png", base_dir.path_join("../misc/logo/icon.png")) == OK,
			"Adding a file to a new subdirectory in the PCK should return an OK error code.");
	CHECK_MESSAGE(
			pck_packer.add_file("some/directories with spaces/to/create/icon.svg", base_dir.path_join("../misc/logo/icon.svg")) == OK,
			"Adding a file to an existing subdirectory in the PCK should return an OK error code.");
	CHECK_MESSAGE(
			pck_packer.add_file("some/directories with spaces/to/create/icon.png", base_dir.path_join("../misc/logo/logo.png")) == OK,
			"Overriding a non-flushed file to an existing subdirectory in the PCK should return an OK error code.");
	CHECK_MESSAGE(
			pck_packer.add_file_from_buffer("buffer/new.txt", String("Hello world!").to_utf8_buffer()) == OK,
			"Adding a file from a buffer to the PCK in a new subdirectory should return an OK error code.");
	CHECK_MESSAGE(
			pck_packer.flush() == OK,
			"Flushing the PCK should return an OK error code.");

	Error err;
	Ref<FileAccess> f = FileAccess::open(output_pck_path, FileAccess::READ, &err);
	CHECK_MESSAGE(
			err == OK,
			"The generated non-empty PCK file should be opened successfully.");
	CHECK_MESSAGE(
			f->get_length() >= 18000,
			"The generated non-empty PCK file should be large enough to actually hold the contents specified above.");
	CHECK_MESSAGE(
			f->get_length() <= 27000,
			"The generated non-empty PCK file shouldn't be too large.");
}

} // namespace TestPCKPacker
