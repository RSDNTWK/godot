#include "tests/test_macros.h"

TEST_FORCE_LINK(test_lzma_properties)

#include "core/io/pck_lzma.h"
#include "tests/core/io/pck_lzma2_fixtures.h"

#include <cstring>

extern "C" {
#include "thirdparty/lzma2605/C/7zAlloc.h"
#include "thirdparty/lzma2605/C/LzmaEnc.h"
}

namespace TestLzmaProperties {

static const ISzAlloc allocator = { SzAlloc, SzFree };

struct ScopedEncoder {
	CLzmaEncHandle handle = LzmaEnc_Create(&allocator);
	~ScopedEncoder() {
		if (handle) {
			LzmaEnc_Destroy(handle, &allocator, &allocator);
		}
	}
};

TEST_CASE("[SDKProperty] Exact dictionary properties preserve every output alignment") {
	ScopedEncoder encoder;
	REQUIRE(encoder.handle != nullptr);
	struct DictionaryCase {
		UInt32 size;
		Byte bytes[4];
	};
	const DictionaryCase dictionaries[] = {
		{ 4096, { 0x00, 0x10, 0x00, 0x00 } },
		{ 1048576, { 0x00, 0x00, 0x10, 0x00 } },
		{ 134217728, { 0x00, 0x00, 0x00, 0x08 } },
	};
	for (const DictionaryCase &dictionary : dictionaries) {
		CLzmaEncProps props;
		LzmaEncProps_Init(&props);
		props.dictSize = dictionary.size;
		props.numThreads = 1;
		REQUIRE(LzmaEnc_SetProps(encoder.handle, &props) == SZ_OK);
		for (int offset = 0; offset < 8; offset++) {
			for (SizeT capacity : { SizeT(5), SizeT(6), SizeT(16) }) {
				alignas(UInt32) Byte buffer[32];
				memset(buffer, 0xa7, sizeof(buffer));
				SizeT size = capacity;
				REQUIRE(LzmaEnc_WriteProperties(encoder.handle, buffer + offset, &size) == SZ_OK);
				CHECK(size == LZMA_PROPS_SIZE);
				CHECK(buffer[offset] == 0x5d); // lc=3, lp=0, pb=2.
				for (int i = 0; i < 4; i++) {
					CHECK(buffer[offset + 1 + i] == dictionary.bytes[i]);
				}
				for (int i = 0; i < int(sizeof(buffer)); i++) {
					if (i < offset || i >= offset + LZMA_PROPS_SIZE) {
						CHECK(buffer[i] == 0xa7);
					}
				}
			}
		}
	}
}

TEST_CASE("[SDKProperty] Short property buffers fail without changing canaries or size") {
	ScopedEncoder encoder;
	REQUIRE(encoder.handle != nullptr);
	CLzmaEncProps props;
	LzmaEncProps_Init(&props);
	props.dictSize = 1048576;
	props.numThreads = 1;
	REQUIRE(LzmaEnc_SetProps(encoder.handle, &props) == SZ_OK);
	for (int offset = 0; offset < 8; offset++) {
		for (SizeT capacity = 0; capacity < LZMA_PROPS_SIZE; capacity++) {
			alignas(UInt32) Byte buffer[32];
			memset(buffer, 0xa7, sizeof(buffer));
			SizeT size = capacity;
			CHECK(LzmaEnc_WriteProperties(encoder.handle, buffer + offset, &size) == SZ_ERROR_PARAM);
			CHECK(size == capacity);
			for (Byte value : buffer) {
				CHECK(value == 0xa7);
			}
		}
	}
}

TEST_CASE("[SDKProperty] Encoded streams match preserved SDK bytes and round trip") {
	struct Fixture {
		int payload_size;
		const uint8_t *encoded;
		int encoded_size;
	};
	using namespace TestPCKSafetyData;
	const Fixture fixtures[] = {
		{ 1031, data_1031, sizeof(data_1031) },
		{ 2051, data_2051, sizeof(data_2051) },
		{ 3119, data_3119, sizeof(data_3119) },
		{ 4099, data_4099, sizeof(data_4099) },
		{ 5003, data_5003, sizeof(data_5003) },
		{ 100003, data_100003, sizeof(data_100003) },
	};
	for (const Fixture &fixture : fixtures) {
		Vector<uint8_t> payload;
		REQUIRE(payload.resize(fixture.payload_size) == OK);
		for (int i = 0; i < payload.size(); i++) {
			payload.write[i] = i % 13;
		}
		PCKLzmaOptions options;
		options.dictionary_size_mb = 1;
		Vector<uint8_t> encoded;
		REQUIRE(compress_lzma2(payload, encoded, options) == OK);
		REQUIRE(encoded.size() == fixture.encoded_size);
		CHECK(memcmp(encoded.ptr(), fixture.encoded, fixture.encoded_size) == 0);
		Vector<uint8_t> decoded;
		REQUIRE(decompress_lzma2(encoded.ptr(), encoded.size(), payload.size(), decoded) == OK);
		CHECK(decoded == payload);
	}
}

} // namespace TestLzmaProperties
