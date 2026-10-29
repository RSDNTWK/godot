/**************************************************************************/
/*  test_range_coder_seed.cpp                                             */
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

#include "core/templates/vector.h"
#include "tests/test_macros.h"

#include <enet/enet.h>

#include <cstring>

namespace TestENetRangeSeed {
bool bundled_range_coder() {
#ifdef GODOT_ENET
	return true;
#else
	return false;
#endif
}
struct Context {
	void *value = enet_range_coder_create();
	~Context() {
		if (value) {
			enet_range_coder_destroy(value);
		}
	}
};

void high_byte_round_trips() {
	static_assert(sizeof(enet_uint32) == 4);
	bool high_seed_seen = false;
	for (uint8_t first : { uint8_t(0), uint8_t(0x7f), uint8_t(0x80), uint8_t(0x83), uint8_t(0xff) }) {
		Context context;
		REQUIRE(context.value != nullptr);
		Vector<uint8_t> payload;
		REQUIRE(payload.resize(513) == OK);
		for (int i = 0; i < payload.size(); i++) {
			payload.write[i] = i == 0 ? first : uint8_t((i / 16) ^ first);
		}
		ENetBuffer input = {};
		input.data = payload.ptrw();
		input.dataLength = payload.size();
		Vector<uint8_t> compressed;
		REQUIRE(compressed.resize(4096) == OK);
		const size_t size = enet_range_coder_compress(context.value, &input, 1, payload.size(), compressed.ptrw(), compressed.size());
		REQUIRE(size > 0);
		REQUIRE(size <= size_t(compressed.size()));
		compressed.resize(size);
		high_seed_seen = high_seed_seen || compressed[0] >= 0x80;
		Vector<uint8_t> repeated;
		REQUIRE(repeated.resize(4096) == OK);
		const size_t repeated_size = enet_range_coder_compress(context.value, &input, 1, payload.size(), repeated.ptrw(), repeated.size());
		REQUIRE(repeated_size == size);
		repeated.resize(repeated_size);
		CHECK(repeated == compressed);
		Vector<uint8_t> output;
		REQUIRE(output.resize(payload.size() + 2) == OK);
		memset(output.ptrw(), 0xa7, output.size());
		const size_t decoded = enet_range_coder_decompress(context.value, compressed.ptr(), compressed.size(), output.ptrw() + 1, payload.size());
		REQUIRE(decoded == size_t(payload.size()));
		CHECK(output[0] == 0xa7);
		CHECK(output[output.size() - 1] == 0xa7);
		CHECK(memcmp(output.ptr() + 1, payload.ptr(), payload.size()) == 0);
	}
	CHECK(high_seed_seen);
}
void bounded_seed_prefixes() {
	for (uint8_t first : { uint8_t(0), uint8_t(0x7f), uint8_t(0x80), uint8_t(0xff) }) {
		Context context;
		REQUIRE(context.value != nullptr);
		const uint8_t seed[] = { first, 0x12, 0x34, 0x56 };
		for (size_t length = 0; length <= sizeof(seed); length++) {
			uint8_t destination[10];
			memset(destination, 0xa7, sizeof(destination));
			CHECK(enet_range_coder_decompress(context.value, seed, length, destination + 5, 0) == 0);
			for (uint8_t byte : destination) {
				CHECK(byte == 0xa7);
			}
		}
	}
}
} // namespace TestENetRangeSeed
