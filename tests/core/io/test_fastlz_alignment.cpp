#include "tests/test_macros.h"
TEST_FORCE_LINK(test_fastlz_alignment)

#include "core/templates/vector.h"
#include "thirdparty/misc/fastlz.h"

#include <cstring>

namespace TestFastLZAlignment {

TEST_CASE("[N006][FastLZ] Existing literal streams keep their bytes and decode") {
	alignas(uint32_t) uint8_t input[24] = {};
	for (int i = 0; i < 16; i++) {
		input[i + 1] = i;
	}
	for (int level : { 1, 2 }) {
		uint8_t encoded[80] = {};
		REQUIRE(fastlz_compress_level(level, input + 1, 16, encoded + 1) == 17);
		CHECK(encoded[1] == (level == 1 ? 0x0f : 0x2f));
		CHECK(memcmp(encoded + 2, input + 1, 16) == 0);
		uint8_t decoded[24] = {};
		REQUIRE(fastlz_decompress(encoded + 1, 17, decoded + 3, 16) == 16);
		CHECK(memcmp(decoded + 3, input + 1, 16) == 0);
	}
}

TEST_CASE("[N006][FastLZ] Valid payloads preserve bytes across source and destination alignments") {
	for (int length : { 16, 17, 31, 32, 33, 64, 127, 256, 1024, 8193, 65535, 65536, 65537, 70001 }) {
		const int capacity = MAX(66, length + length / 20 + 16);
		for (int pattern = 0; pattern < 3; pattern++) {
			Vector<uint8_t> payload;
			REQUIRE(payload.resize(length) == OK);
			uint32_t state = 0x82ff0001;
			for (int i = 0; i < length; i++) {
				state = state * 1664525U + 1013904223U;
				payload.write[i] = pattern == 0 ? uint8_t(state >> 24) : (pattern == 1 ? uint8_t(i % 13) : uint8_t(0xff));
			}
			for (int level : { 0, 1, 2 }) {
				Vector<uint8_t> reference;
				REQUIRE(reference.resize(capacity) == OK);
				const int expected = level == 0 ? fastlz_compress(payload.ptr(), length, reference.ptrw()) : fastlz_compress_level(level, payload.ptr(), length, reference.ptrw());
				REQUIRE(expected > 0);
				REQUIRE(expected <= capacity);
				for (int offset = 0; offset < 8; offset++) {
					const int source_offset = 8 + offset;
					const int destination_offset = 8 + ((offset * 3 + 1) % 8);
					Vector<uint8_t> input;
					Vector<uint8_t> encoded;
					Vector<uint8_t> decoded;
					REQUIRE(input.resize(length + 32) == OK);
					REQUIRE(encoded.resize(capacity + 32) == OK);
					REQUIRE(decoded.resize(length + 32) == OK);
					memset(input.ptrw(), 0xa7, input.size());
					memset(encoded.ptrw(), 0xa7, encoded.size());
					memset(decoded.ptrw(), 0xa7, decoded.size());
					memcpy(input.ptrw() + source_offset, payload.ptr(), length);
					const int size = level == 0 ? fastlz_compress(input.ptr() + source_offset, length, encoded.ptrw() + destination_offset) : fastlz_compress_level(level, input.ptr() + source_offset, length, encoded.ptrw() + destination_offset);
					REQUIRE(size == expected);
					CHECK(memcmp(encoded.ptr() + destination_offset, reference.ptr(), size) == 0);
					REQUIRE(fastlz_decompress(encoded.ptr() + destination_offset, size, decoded.ptrw() + source_offset, length) == length);
					CHECK(memcmp(decoded.ptr() + source_offset, payload.ptr(), length) == 0);
					CHECK(memcmp(input.ptr() + source_offset, payload.ptr(), length) == 0);
					for (int i = 0; i < destination_offset; i++) {
						CHECK(encoded[i] == 0xa7);
					}
					for (int i = destination_offset + capacity; i < encoded.size(); i++) {
						CHECK(encoded[i] == 0xa7);
					}
					for (int i = 0; i < source_offset; i++) {
						CHECK(decoded[i] == 0xa7);
					}
					for (int i = source_offset + length; i < decoded.size(); i++) {
						CHECK(decoded[i] == 0xa7);
					}
				}
			}
		}
	}
}
} // namespace TestFastLZAlignment
