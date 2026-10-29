#include "tests/test_macros.h"
TEST_FORCE_LINK(test_fastlz_bounds)

#include "core/templates/vector.h"
#include "thirdparty/misc/fastlz.h"
#include <cstring>

namespace TestFastLZBounds {
static Vector<uint8_t> plain(int p_size) {
	Vector<uint8_t> result;
	REQUIRE(result.resize(p_size) == OK);
	uint32_t state = 0x612fa392;
	for (int i = 0; i < p_size; i++) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		result.write[i] = p_size == 513 && i % 8 < 4 ? uint8_t((i % 8) * 17 + 3) : (i >= 16384 ? result[i - 16384] : uint8_t(state));
	}
	return result;
}

TEST_CASE("[N007][FastLZ] Generated stream prefixes cannot depend on following storage") {
	for (int length : { 257, 20003, 513 }) {
		const Vector<uint8_t> input = plain(length);
		for (int level : { 1, 2 }) {
			Vector<uint8_t> encoded;
			REQUIRE(encoded.resize(length + length / 20 + 80) == OK);
			const int size = fastlz_compress_level(level, input.ptr(), input.size(), encoded.ptrw());
			REQUIRE(size > 0);
			if (length == 513) { REQUIRE(size < length); }
			Vector<uint8_t> full;
			REQUIRE(full.resize(length + 16) == OK);
			REQUIRE(fastlz_decompress(encoded.ptr(), size, full.ptrw(), length) == length);
			CHECK(memcmp(full.ptr(), input.ptr(), length) == 0);
			for (int extent = 1; extent < size; extent++) {
				if (length == 20003 && extent > 32 && extent < size - 32 && extent % 137 != 0) {
					continue;
				}
				Vector<uint8_t> first = encoded;
				Vector<uint8_t> second = encoded;
				memset(first.ptrw() + extent, 0, first.size() - extent);
				memset(second.ptrw() + extent, 0xff, second.size() - extent);
				Vector<uint8_t> a;
				Vector<uint8_t> b;
				REQUIRE(a.resize(length + 16) == OK);
				REQUIRE(b.resize(length + 16) == OK);
				memset(a.ptrw(), 0xa7, a.size());
				memset(b.ptrw(), 0xa7, b.size());
				const int one = fastlz_decompress(first.ptr(), extent, a.ptrw(), length);
				const int two = fastlz_decompress(second.ptr(), extent, b.ptrw(), length);
				CHECK(one == two);
				CHECK(one >= 0);
				CHECK(one <= length);
				CHECK(a == b);
				for (int i = length; i < a.size(); i++) {
					CHECK(a[i] == 0xa7);
				}
				if (one > 0) {
					CHECK(memcmp(a.ptr(), input.ptr(), one) == 0);
				}
			}
		}
	}
}

TEST_CASE("[N007][FastLZ] Empty invalid capacities and short destinations fail safely") {
	const Vector<uint8_t> input = plain(257);
	Vector<uint8_t> encoded;
	REQUIRE(encoded.resize(400) == OK);
	for (int level : { 1, 2 }) {
		const int size = fastlz_compress_level(level, input.ptr(), input.size(), encoded.ptrw());
		REQUIRE(size > 0);
		Vector<uint8_t> output;
		REQUIRE(output.resize(300) == OK);
		memset(output.ptrw(), 0xa7, output.size());
		CHECK(fastlz_decompress(nullptr, size, output.ptrw(), 257) == 0);
		CHECK(fastlz_decompress(encoded.ptr(), size, nullptr, 257) == 0);
		CHECK(fastlz_decompress(encoded.ptr(), 0, output.ptrw(), 257) == 0);
		CHECK(fastlz_decompress(encoded.ptr(), -1, output.ptrw(), 257) == 0);
		CHECK(fastlz_decompress(encoded.ptr(), size, output.ptrw(), 0) == 0);
		CHECK(fastlz_decompress(encoded.ptr(), size, output.ptrw(), -1) == 0);
		CHECK(fastlz_decompress(encoded.ptr(), size, output.ptrw(), 256) == 0);
		for (int i = 256; i < output.size(); i++) {
			CHECK(output[i] == 0xa7);
		}
	}
}
} // namespace TestFastLZBounds
