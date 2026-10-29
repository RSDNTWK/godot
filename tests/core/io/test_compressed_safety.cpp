#include "tests/test_macros.h"
TEST_FORCE_LINK(test_compressed_safety)

#include "core/io/compression.h"
#include "core/io/file_access_compressed.h"
#include "core/io/file_access_memory.h"
#include "core/io/marshalls.h"
#include <cstring>

namespace TestCompressedSafety {
struct ErrorsOff {
	bool previous = CoreGlobals::print_error_enabled;
	ErrorsOff() { CoreGlobals::print_error_enabled = false; }
	~ErrorsOff() { CoreGlobals::print_error_enabled = previous; }
};

static Vector<uint8_t> payload(int p_size, int p_offset = 0) {
	Vector<uint8_t> data;
	REQUIRE(data.resize(p_size) == OK);
	for (int i = 0; i < p_size; i++) {
		data.write[i] = (i + p_offset) % 251;
	}
	return data;
}

static Vector<uint8_t> stream(Compression::Mode p_mode, uint32_t p_total, uint32_t p_block_size, int p_changed_block = -1, int p_delta = 0) {
	const uint32_t count = p_total / p_block_size + 1;
	Vector<uint8_t> data;
	REQUIRE(data.resize(16 + count * 4) == OK);
	memcpy(data.ptrw(), "GCMP", 4);
	encode_uint32(p_mode, data.ptrw() + 4);
	encode_uint32(p_block_size, data.ptrw() + 8);
	encode_uint32(p_total, data.ptrw() + 12);
	for (uint32_t i = 0; i < count; i++) {
		const int expected = MIN(p_block_size, p_total - i * p_block_size);
		Vector<uint8_t> plain = payload(expected + (int(i) == p_changed_block ? p_delta : 0), i * p_block_size);
		const int64_t bound = Compression::get_max_compressed_buffer_size(plain.size(), p_mode);
		REQUIRE(bound >= 0);
		Vector<uint8_t> encoded;
		REQUIRE(encoded.resize(bound) == OK);
		const int64_t size = Compression::compress(encoded.ptrw(), plain.ptr(), plain.size(), p_mode);
		REQUIRE(size >= 0);
		REQUIRE(size <= bound);
		encode_uint32(size, data.ptrw() + 16 + i * 4);
		const int64_t start = data.size();
		REQUIRE(data.resize(start + size) == OK);
		if (size > 0) {
			memcpy(data.ptrw() + start, encoded.ptr(), size);
		}
	}
	const int64_t end = data.size();
	REQUIRE(data.resize(end + 4) == OK);
	memcpy(data.ptrw() + end, "GCMP", 4);
	return data;
}

static Ref<FileAccessMemory> memory(const Vector<uint8_t> &p_data) {
	Ref<FileAccessMemory> result;
	result.instantiate();
	REQUIRE(result->open_custom(p_data.ptr(), p_data.size()) == OK);
	result->seek(4);
	return result;
}

TEST_CASE("[F10][Compressed] Valid empty, tiny, full and final blocks preserve read and seek behavior") {
	for (Compression::Mode mode : { Compression::MODE_FASTLZ, Compression::MODE_DEFLATE, Compression::MODE_ZSTD, Compression::MODE_GZIP, Compression::MODE_LZMA2 }) {
		for (int length : { 0, 1, 15, 16, 31, 32, 33, 64 }) {
			Vector<uint8_t> data = stream(mode, length, 32);
			FileAccessCompressed reader;
			REQUIRE(reader.open_after_magic(memory(data)) == OK);
			CHECK(reader.get_length() == uint64_t(length));
			CHECK(reader.get_position() == 0);
			Vector<uint8_t> output;
			REQUIRE(output.resize(length + 16) == OK);
			memset(output.ptrw(), 0xa7, output.size());
			CHECK(reader.get_buffer(output.ptrw(), length) == uint64_t(length));
			if (length > 0) {
				const Vector<uint8_t> expected = payload(length);
				CHECK(memcmp(output.ptr(), expected.ptr(), length) == 0);
			}
			CHECK(reader.get_error() == OK);
			CHECK(reader.get_position() == uint64_t(length));
			CHECK(reader.get_buffer(output.ptrw(), 1) == 0);
			CHECK(reader.eof_reached());
			CHECK(reader.get_error() == ERR_FILE_EOF);
			reader.seek(0);
			CHECK_FALSE(reader.eof_reached());
			CHECK(reader.get_error() == OK);
			CHECK(reader.get_buffer(output.ptrw(), length + 1) == uint64_t(length));
			CHECK(reader.eof_reached());
			reader.seek(length);
			CHECK(reader.get_position() == uint64_t(length));
			CHECK_FALSE(reader.eof_reached());
			if (length > 0) {
				reader.seek_end(-1);
				CHECK(reader.get_position() == uint64_t(length - 1));
				CHECK(reader.get_buffer(output.ptrw(), 1) == 1);
				CHECK(output[0] == uint8_t((length - 1) % 251));
			}
		}
	}
}

TEST_CASE("[F10][Compressed] Incomplete metadata and unsupported extents fail without opening") {
	ErrorsOff quiet;
	const Vector<uint8_t> original = stream(Compression::MODE_ZSTD, 8, 32);
	for (int length : { 4, 8, 15, 16, 19, 20, int(original.size() - 5) }) {
		Vector<uint8_t> data = original;
		REQUIRE(data.resize(length) == OK);
		FileAccessCompressed reader;
		CHECK(reader.open_after_magic(memory(data)) == ERR_FILE_CORRUPT);
		CHECK_FALSE(reader.is_open());
	}
	for (int variant = 0; variant < 5; variant++) {
		Vector<uint8_t> data = original;
		if (variant == 0) { encode_uint32(0, data.ptrw() + 8); }
		if (variant == 1) { encode_uint32(0xffffffff, data.ptrw() + 4); }
		if (variant == 2) {
			encode_uint32(1, data.ptrw() + 8);
			encode_uint32(0xffffffff, data.ptrw() + 12);
		}
		if (variant == 3) { encode_uint32(0xffffffff, data.ptrw() + 16); }
		if (variant == 4) { encode_uint32(0, data.ptrw() + 16); }
		FileAccessCompressed reader;
		CHECK(reader.open_after_magic(memory(data)) == ERR_FILE_CORRUPT);
		CHECK_FALSE(reader.is_open());
	}
}

TEST_CASE("[F10][Compressed] Decoded block size must match declared logical data") {
	ErrorsOff quiet;
	for (int index : { 0, 1, 2 }) {
		for (int delta : { -1, 1 }) {
			Vector<uint8_t> data = stream(Compression::MODE_ZSTD, 71, 32, index, delta);
			FileAccessCompressed reader;
			const Error opened = reader.open_after_magic(memory(data));
			if (index == 0) {
				CHECK(opened == ERR_FILE_CORRUPT);
				CHECK_FALSE(reader.is_open());
			} else {
				REQUIRE(opened == OK);
				uint8_t output[96];
				memset(output, 0xa7, sizeof(output));
				const uint64_t prefix = index * 32;
				CHECK(reader.get_buffer(output, sizeof(output)) == prefix);
				CHECK(reader.get_position() == prefix);
				CHECK(reader.get_error() == ERR_FILE_CORRUPT);
				const Vector<uint8_t> expected = payload(prefix);
				CHECK(memcmp(output, expected.ptr(), prefix) == 0);
				for (uint64_t i = prefix; i < sizeof(output); i++) {
					CHECK(output[i] == 0xa7);
				}
				CHECK(reader.get_buffer(output, 1) == 0);
				reader.seek(0);
				CHECK(reader.get_error() == ERR_FILE_CORRUPT);
				CHECK(reader.get_buffer(output, 1) == 0);
			}
		}
	}
}

class PartialReader : public FileAccessMemory {
public:
	uint64_t limit = UINT64_MAX;
	mutable bool failed = false;
	uint64_t get_buffer(uint8_t *p_dst, uint64_t p_length) const override {
		const uint64_t actual = FileAccessMemory::get_buffer(p_dst, MIN(p_length, limit));
		failed = actual < p_length;
		return actual;
	}
	Error get_error() const override { return failed ? ERR_FILE_CANT_READ : OK; }
};

TEST_CASE("[F10][Compressed] A short backing read preserves only an already validated prefix") {
	Vector<uint8_t> data = stream(Compression::MODE_ZSTD, 71, 32);
	Ref<PartialReader> backing;
	backing.instantiate();
	REQUIRE(backing->open_custom(data.ptr(), data.size()) == OK);
	backing->seek(4);
	FileAccessCompressed reader;
	REQUIRE(reader.open_after_magic(backing) == OK);
	backing->limit = 2;
	uint8_t output[96];
	memset(output, 0xa7, sizeof(output));
	CHECK(reader.get_buffer(output, sizeof(output)) == 32);
	CHECK(reader.get_error() == ERR_FILE_CANT_READ);
	CHECK(reader.get_position() == 32);
	for (int i = 32; i < int(sizeof(output)); i++) {
		CHECK(output[i] == 0xa7);
	}
	CHECK(reader.get_buffer(output, sizeof(output)) == 0);
	reader.close();
	backing->limit = UINT64_MAX;
	backing->failed = false;
	backing->seek(4);
	REQUIRE(reader.open_after_magic(backing) == OK);
	CHECK(reader.get_buffer(output, 71) == 71);
	CHECK(reader.get_error() == OK);
}

TEST_CASE("[F10][Compression] Zstd failures have a stable minus-one result") {
	ErrorsOff quiet;
	const uint8_t input[3] = { 0, 1, 2 };
	uint8_t output[32];
	memset(output, 0xa7, sizeof(output));
	CHECK(Compression::decompress(output, sizeof(output), input, sizeof(input), Compression::MODE_ZSTD) == -1);
	for (uint8_t value : output) {
		CHECK(value == 0xa7);
	}
	CHECK(Compression::decompress(output, -1, input, sizeof(input), Compression::MODE_ZSTD) == -1);
	CHECK(Compression::decompress(output, sizeof(output), input, -1, Compression::MODE_ZSTD) == -1);
}

TEST_CASE("[F10][Compression] Failed tiny FastLZ decoding does not copy temporary bytes") {
	ErrorsOff quiet;
	const uint8_t input[1] = { 0xe0 };
	uint8_t output[32];
	for (int capacity : { 0, 1, 15, 16 }) {
		memset(output, 0xa7, sizeof(output));
		CHECK(Compression::decompress(output, capacity, input, sizeof(input), Compression::MODE_FASTLZ) == -1);
		for (uint8_t value : output) {
			CHECK(value == 0xa7);
		}
	}
	CHECK(Compression::decompress(output, 1, nullptr, 0, Compression::MODE_FASTLZ) == -1);
	CHECK(Compression::decompress(output, 1, input, int64_t(INT32_MAX) + 1, Compression::MODE_FASTLZ) == -1);
}
} // namespace TestCompressedSafety
