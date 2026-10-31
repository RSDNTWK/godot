/**************************************************************************/
/*  test_ktx_dispatch.cpp                                                  */
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
TEST_FORCE_LINK(test_ktx_dispatch)
#include "modules/modules_enabled.gen.h"
#ifdef MODULE_KTX_ENABLED
#include "ktx.h"
#include "thirdparty/libktx/lib/texture.h"

#include "core/io/image.h"
#include "core/io/marshalls.h"

#include <cstring>
namespace TestKtxDispatch {
struct TextureOwner {
	ktxTexture *texture = nullptr;
	~TextureOwner() {
		if (texture) {
			ktxTexture_Destroy(texture);
		}
	}
};
static KTX_error_code count_levels(int p_level, int p_face, int p_width, int p_height, int p_depth, ktx_uint64_t p_size, void *p_pixels, void *p_userdata) {
	CHECK(p_level >= 0);
	CHECK(p_face == 0);
	CHECK(p_width > 0);
	CHECK(p_height > 0);
	CHECK(p_depth > 0);
	CHECK(p_size > 0);
	CHECK(p_pixels != nullptr);
	++*static_cast<int *>(p_userdata);
	return KTX_SUCCESS;
}
TEST_CASE("[R4][KTX] Every public and internal KTX1 and KTX2 dispatch slot has its declared type") {
	for (int version : { 1, 2 }) {
		ktxTextureCreateInfo info = {};
		info.glInternalformat = 0x8058; // GL_RGBA8.
		info.vkFormat = 37; // VK_FORMAT_R8G8B8A8_UNORM.
		info.baseWidth = info.baseHeight = 4;
		info.baseDepth = 1;
		info.numDimensions = 2;
		info.numLevels = 3;
		info.numLayers = info.numFaces = 1;
		TextureOwner owner;
		KTX_error_code error;
		if (version == 1) {
			ktxTexture1 *texture = nullptr;
			error = ktxTexture1_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture);
			owner.texture = reinterpret_cast<ktxTexture *>(texture);
		} else {
			ktxTexture2 *texture = nullptr;
			error = ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture);
			owner.texture = reinterpret_cast<ktxTexture *>(texture);
		}
		REQUIRE(error == KTX_SUCCESS);
		REQUIRE(owner.texture != nullptr);
		if (!owner.texture) {
			continue;
		}
		ktxTexture *texture = owner.texture;
		REQUIRE(texture->pData != nullptr);
		if (!texture->pData) {
			continue;
		}
		memset(texture->pData, 0x11, texture->dataSize);
		CHECK(ktxTexture_GetDataSizeUncompressed(texture) == 84);
		CHECK(ktxTexture_GetImageSize(texture, 0) == 64);
		CHECK(ktxTexture_GetLevelSize(texture, 0) == 64);
		ktx_size_t offset = 0;
		CHECK(ktxTexture_GetImageOffset(texture, 0, 0, 0, &offset) == KTX_SUCCESS);
		CHECK(texture->_protected->_vtbl.calcDataSizeLevels(texture, 3) == 84);
		CHECK(texture->_protected->_vtbl.calcFaceLodSize(texture, 0) == 64);
		CHECK(texture->_protected->_vtbl.calcLevelOffset(texture, 0) == offset);
		CHECK_FALSE(ktxTexture_NeedsTranscoding(texture));
		int count = 0;
		CHECK(ktxTexture_IterateLevels(texture, count_levels, &count) == KTX_SUCCESS);
		CHECK(count == 3);
		CHECK(ktxTexture_IterateLoadLevelFaces(texture, count_levels, &count) == KTX_INVALID_OPERATION);
		CHECK(ktxTexture_LoadImageData(texture, nullptr, 0) == KTX_INVALID_OPERATION);
		const uint8_t pixel[] = { 1, 2, 3, 255 };
		CHECK(ktxTexture_SetImageFromMemory(texture, 0, 0, 0, pixel, sizeof(pixel)) == KTX_INVALID_OPERATION);
		CHECK(ktxTexture_SetImageFromStdioStream(texture, 0, 0, 0, nullptr, 0) == KTX_INVALID_OPERATION);
		CHECK(ktxTexture_WriteToStdioStream(texture, nullptr) == KTX_INVALID_OPERATION);
		CHECK(ktxTexture_WriteToNamedFile(texture, nullptr) == KTX_INVALID_OPERATION);
		uint8_t *output = nullptr;
		ktx_size_t size = 0;
		CHECK(ktxTexture_WriteToMemory(texture, &output, &size) == KTX_INVALID_OPERATION);
		CHECK(ktxTexture_WriteToStream(texture, nullptr) == KTX_INVALID_OPERATION);
		CHECK(output == nullptr);
	}
}

static Vector<uint8_t> stream_fixture(int p_version) {
	Vector<uint8_t> data;
	const uint8_t identifier1[] = { 0xab, 0x4b, 0x54, 0x58, 0x20, 0x31, 0x31, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a };
	const uint8_t identifier2[] = { 0xab, 0x4b, 0x54, 0x58, 0x20, 0x32, 0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a };
	if (p_version == 1) {
		REQUIRE(data.resize(72) == OK);
		data.fill(0);
		memcpy(data.ptrw(), identifier1, 12);
		const uint32_t fields[] = { 0x04030201, 0x1401, 1, 0x1908, 0x8058, 0x1908, 1, 1, 0, 0, 1, 1, 0, 4 };
		for (int i = 0; i < 14; ++i) {
			encode_uint32(fields[i], data.ptrw() + 12 + i * 4);
		}
	} else {
		ktxTextureCreateInfo info = {};
		info.vkFormat = 37;
		info.baseWidth = info.baseHeight = info.baseDepth = 1;
		info.numDimensions = 2;
		info.numLevels = info.numLayers = info.numFaces = 1;
		ktxTexture2 *texture = nullptr;
		REQUIRE(ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_NO_STORAGE, &texture) == KTX_SUCCESS);
		REQUIRE(texture != nullptr);
		if (!texture) {
			return data;
		}
		const uint32_t dfd_size = texture->pDfd[0];
		const uint64_t image_offset = (104 + uint64_t(dfd_size) + 7) & ~uint64_t(7);
		REQUIRE(data.resize(image_offset + 4) == OK);
		data.fill(0);
		memcpy(data.ptrw(), identifier2, 12);
		encode_uint32(37, data.ptrw() + 12);
		encode_uint32(1, data.ptrw() + 16);
		encode_uint32(1, data.ptrw() + 20);
		encode_uint32(1, data.ptrw() + 24);
		encode_uint32(1, data.ptrw() + 36);
		encode_uint32(1, data.ptrw() + 40);
		encode_uint32(104, data.ptrw() + 48);
		encode_uint32(dfd_size, data.ptrw() + 52);
		encode_uint64(image_offset, data.ptrw() + 80);
		encode_uint64(4, data.ptrw() + 88);
		encode_uint64(4, data.ptrw() + 96);
		memcpy(data.ptrw() + 104, texture->pDfd, dfd_size);
		ktxTexture2_Destroy(texture);
	}
	const uint8_t pixel[] = { 12, 34, 56, 255 };
	memcpy(data.ptrw() + data.size() - 4, pixel, 4);
	return data;
}
TEST_CASE("[R4][KTX] Delayed KTX1 and KTX2 memory streams load and iterate through base dispatch") {
	for (int version : { 1, 2 }) {
		const Vector<uint8_t> data = stream_fixture(version);
		REQUIRE(!data.is_empty());
		for (bool iterate : { false, true }) {
			TextureOwner owner;
			REQUIRE(ktxTexture_CreateFromMemory(data.ptr(), data.size(), KTX_TEXTURE_CREATE_NO_FLAGS, &owner.texture) == KTX_SUCCESS);
			REQUIRE(owner.texture != nullptr);
			if (!owner.texture) {
				continue;
			}
			CHECK(owner.texture->pData == nullptr);
			if (iterate) {
				int count = 0;
				CHECK(ktxTexture_IterateLoadLevelFaces(owner.texture, count_levels, &count) == KTX_SUCCESS);
				CHECK(count == 1);
			} else {
				CHECK(ktxTexture_LoadImageData(owner.texture, nullptr, 0) == KTX_SUCCESS);
				REQUIRE(owner.texture->pData != nullptr);
				if (owner.texture->pData) {
					const uint8_t pixel[] = { 12, 34, 56, 255 };
					CHECK(memcmp(owner.texture->pData, pixel, 4) == 0);
				}
			}
		}
		Ref<Image> image;
		image.instantiate();
		if (version == 1) {
			const Error error = image->load_ktx_from_buffer(data);
			REQUIRE(error == OK);
			if (error != OK) {
				continue;
			}
			CHECK(image->get_width() == 1);
			CHECK(image->get_height() == 1);
			CHECK(image->get_data() == data.slice(data.size() - 4));
		} else {
			// The engine adapter supports compressed/Basis KTX2, not raw RGBA8.
			// The library stream controls above still exercise raw KTX2 dispatch.
			ERR_PRINT_OFF;
			const Error error = image->load_ktx_from_buffer(data);
			ERR_PRINT_ON;
			CHECK(error == ERR_PARSE_ERROR);
			CHECK(image->is_empty());
		}
	}
}
} // namespace TestKtxDispatch
#endif
