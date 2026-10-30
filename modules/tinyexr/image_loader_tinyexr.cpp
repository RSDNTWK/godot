/**************************************************************************/
/*  image_loader_tinyexr.cpp                                              */
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

#include "image_loader_tinyexr.h"

#include "exr_image_size.h"

#include "core/io/file_access_memory.h"

#include <zlib.h>
// zlib should come before including tinyexr.
#include <thirdparty/tinyexr/tinyexr.h>

Error ImageLoaderTinyEXR::load_image(Ref<Image> p_image, Ref<FileAccess> f, BitField<ImageFormatLoader::LoaderFlags> p_flags, float p_scale) {
	Vector<uint8_t> src_image;
	uint64_t src_image_len = f->get_length();
	ERR_FAIL_COND_V(src_image_len == 0, ERR_FILE_CORRUPT);
	ERR_FAIL_COND_V(src_image_len > MIN(uint64_t(INT64_MAX), uint64_t(SIZE_MAX)), ERR_FILE_CORRUPT);
	const Error allocation = src_image.resize(src_image_len);
	ERR_FAIL_COND_V(allocation != OK, allocation);
	uint8_t *w = src_image.ptrw();
	const uint64_t read = f->get_buffer(w, src_image_len);
	const Error read_error = f->get_error();
	ERR_FAIL_COND_V(read_error != OK && read_error != ERR_FILE_EOF, read_error);
	ERR_FAIL_COND_V(read != src_image_len, ERR_FILE_CORRUPT);

	// Re-implementation of tinyexr's LoadEXRFromMemory using Godot types to store the Image data
	// and Godot's error codes.
	// When debugging after updating the thirdparty library, check that we're still in sync with
	// their API usage in LoadEXRFromMemory.

	EXRVersion exr_version;
	struct EXRResources {
		EXRImage image;
		EXRHeader header;
		EXRResources() {
			InitEXRImage(&image);
			InitEXRHeader(&header);
		}
		~EXRResources() {
			FreeEXRImage(&image);
			FreeEXRHeader(&header);
		}
	} resources;
	EXRImage &exr_image = resources.image;
	EXRHeader &exr_header = resources.header;
	const char *err = nullptr;

	int ret = ParseEXRVersionFromMemory(&exr_version, w, src_image_len);
	if (ret != TINYEXR_SUCCESS) {
		return ERR_FILE_CORRUPT;
	}

	ret = ParseEXRHeaderFromMemory(&exr_header, &exr_version, w, src_image_len, &err);
	if (ret != TINYEXR_SUCCESS) {
		if (err) {
			ERR_PRINT(String(err));
			FreeEXRErrorMessage(err);
		}
		return ERR_FILE_CORRUPT;
	}

	int64_t validated_bytes = 0;
	const Error dimensions_error = EXRImageSize::output_bytes(
			int64_t(exr_header.data_window.max_x) - exr_header.data_window.min_x + 1,
			int64_t(exr_header.data_window.max_y) - exr_header.data_window.min_y + 1, 1, 2, validated_bytes);
	ERR_FAIL_COND_V(dimensions_error != OK, dimensions_error);

	// Read HALF channel as FLOAT. (GH-13490)
	bool use_float16 = false;
	for (int i = 0; i < exr_header.num_channels; i++) {
		if (exr_header.pixel_types[i] == TINYEXR_PIXELTYPE_HALF) {
			use_float16 = true;
			exr_header.requested_pixel_types[i] = TINYEXR_PIXELTYPE_FLOAT;
		}
	}

	ret = LoadEXRImageFromMemory(&exr_image, &exr_header, w, src_image_len, &err);
	if (ret != TINYEXR_SUCCESS) {
		if (err) {
			ERR_PRINT(String(err));
			FreeEXRErrorMessage(err);
		}
		return ERR_FILE_CORRUPT;
	}

	// RGBA
	int idxR = -1;
	int idxG = -1;
	int idxB = -1;
	int idxA = -1;
	for (int c = 0; c < exr_header.num_channels; c++) {
		if (strcmp(exr_header.channels[c].name, "R") == 0) {
			idxR = c;
		} else if (strcmp(exr_header.channels[c].name, "G") == 0) {
			idxG = c;
		} else if (strcmp(exr_header.channels[c].name, "B") == 0) {
			idxB = c;
		} else if (strcmp(exr_header.channels[c].name, "A") == 0) {
			idxA = c;
		} else if (strcmp(exr_header.channels[c].name, "Y") == 0) {
			idxR = c;
			idxG = c;
			idxB = c;
		}
	}

	// EXR image data loaded, now parse it into Godot-friendly image data

	Vector<uint8_t> imgdata;
	Image::Format format;
	int output_channels = 0;

	int channel_size = use_float16 ? 2 : 4;
	if (idxA != -1) {
		ERR_FAIL_COND_V(idxR == -1 || idxG == -1 || idxB == -1, ERR_FILE_CORRUPT);
		format = use_float16 ? Image::FORMAT_RGBAH : Image::FORMAT_RGBAF;
		output_channels = 4;
	} else if (idxB != -1) {
		ERR_FAIL_COND_V(idxG == -1, ERR_FILE_CORRUPT);
		ERR_FAIL_COND_V(idxR == -1, ERR_FILE_CORRUPT);
		format = use_float16 ? Image::FORMAT_RGBH : Image::FORMAT_RGBF;
		output_channels = 3;
	} else if (idxG != -1) {
		ERR_FAIL_COND_V(idxR == -1, ERR_FILE_CORRUPT);
		format = use_float16 ? Image::FORMAT_RGH : Image::FORMAT_RGF;
		output_channels = 2;
	} else {
		ERR_FAIL_COND_V(idxR == -1, ERR_FILE_CORRUPT);
		format = use_float16 ? Image::FORMAT_RH : Image::FORMAT_RF;
		output_channels = 1;
	}

	int64_t output_size = 0;
	const Error size_error = EXRImageSize::output_bytes(exr_image.width, exr_image.height, output_channels, channel_size, output_size);
	ERR_FAIL_COND_V(size_error != OK, size_error);
	const Error output_allocation = imgdata.resize(output_size);
	ERR_FAIL_COND_V(output_allocation != OK, output_allocation);

	EXRTile single_image_tile;
	int num_tiles;
	int tile_width = 0;
	int tile_height = 0;

	const EXRTile *exr_tiles;

	if (!exr_header.tiled) {
		single_image_tile.images = exr_image.images;
		single_image_tile.width = exr_image.width;
		single_image_tile.height = exr_image.height;
		single_image_tile.level_x = exr_image.width;
		single_image_tile.level_y = exr_image.height;
		single_image_tile.offset_x = 0;
		single_image_tile.offset_y = 0;

		exr_tiles = &single_image_tile;
		num_tiles = 1;
		tile_width = exr_image.width;
		tile_height = exr_image.height;
	} else {
		tile_width = exr_header.tile_size_x;
		tile_height = exr_header.tile_size_y;
		num_tiles = exr_image.num_tiles;
		exr_tiles = exr_image.tiles;
	}

	//print_line("reading format: " + Image::get_format_name(format));
	{
		uint8_t *wd = imgdata.ptrw();
		uint16_t *iw16 = (uint16_t *)wd;
		float *iw32 = (float *)wd;

		const auto read_sample = [](const uint8_t *&r_channel, int p_pixel_type) {
			float result;
			if (p_pixel_type == TINYEXR_PIXELTYPE_UINT) {
				uint32_t value;
				memcpy(&value, r_channel, sizeof(value));
				result = float(value);
			} else {
				memcpy(&result, r_channel, sizeof(result));
			}
			r_channel += sizeof(uint32_t);
			return result;
		};

		// Assume `out_rgba` have enough memory allocated.
		for (int tile_index = 0; tile_index < num_tiles; tile_index++) {
			const EXRTile &tile = exr_tiles[tile_index];

			int tw = tile.width;
			int th = tile.height;
			int64_t tile_x, tile_y;
			ERR_FAIL_COND_V(EXRImageSize::tile_rectangle(exr_image.width, exr_image.height, tile_width, tile_height,
									tile.offset_x, tile.offset_y, tw, th, tile_x, tile_y) != OK,
					ERR_FILE_CORRUPT);

			const uint8_t *r_channel_start = reinterpret_cast<const uint8_t *>(tile.images[idxR]);
			const uint8_t *g_channel_start = nullptr;
			const uint8_t *b_channel_start = nullptr;
			const uint8_t *a_channel_start = nullptr;

			if (idxG != -1) {
				g_channel_start = reinterpret_cast<const uint8_t *>(tile.images[idxG]);
			}
			if (idxB != -1) {
				b_channel_start = reinterpret_cast<const uint8_t *>(tile.images[idxB]);
			}
			if (idxA != -1) {
				a_channel_start = reinterpret_cast<const uint8_t *>(tile.images[idxA]);
			}

			const int64_t output_offset = (tile_y * exr_image.width + tile_x) * output_channels;
			uint16_t *first_row_w16 = use_float16 ? iw16 + output_offset : nullptr;
			float *first_row_w32 = use_float16 ? nullptr : iw32 + output_offset;

			for (int y = 0; y < th; y++) {
				const uint8_t *r_channel = r_channel_start + int64_t(y) * tile_width * sizeof(uint32_t);
				const uint8_t *g_channel = nullptr;
				const uint8_t *b_channel = nullptr;
				const uint8_t *a_channel = nullptr;
				if (g_channel_start) {
					g_channel = g_channel_start + int64_t(y) * tile_width * sizeof(uint32_t);
				}
				if (b_channel_start) {
					b_channel = b_channel_start + int64_t(y) * tile_width * sizeof(uint32_t);
				}
				if (a_channel_start) {
					a_channel = a_channel_start + int64_t(y) * tile_width * sizeof(uint32_t);
				}

				if (use_float16) {
					uint16_t *row_w = first_row_w16 + (int64_t(y) * exr_image.width * output_channels);

					for (int x = 0; x < tw; x++) {
						Color color;
						color.r = read_sample(r_channel, exr_header.pixel_types[idxR]);
						if (g_channel) {
							color.g = read_sample(g_channel, exr_header.pixel_types[idxG]);
						}
						if (b_channel) {
							color.b = read_sample(b_channel, exr_header.pixel_types[idxB]);
						}
						if (a_channel) {
							color.a = read_sample(a_channel, exr_header.pixel_types[idxA]);
						}

						if (p_flags & FLAG_FORCE_LINEAR) {
							color = color.srgb_to_linear();
						}

						*row_w++ = Math::make_half_float(color.r);
						if (g_channel) {
							*row_w++ = Math::make_half_float(color.g);
						}
						if (b_channel) {
							*row_w++ = Math::make_half_float(color.b);
						}
						if (a_channel) {
							*row_w++ = Math::make_half_float(color.a);
						}
					}
				} else {
					float *row_w = first_row_w32 + (int64_t(y) * exr_image.width * output_channels);

					for (int x = 0; x < tw; x++) {
						Color color;
						color.r = read_sample(r_channel, exr_header.pixel_types[idxR]);
						if (g_channel) {
							color.g = read_sample(g_channel, exr_header.pixel_types[idxG]);
						}
						if (b_channel) {
							color.b = read_sample(b_channel, exr_header.pixel_types[idxB]);
						}
						if (a_channel) {
							color.a = read_sample(a_channel, exr_header.pixel_types[idxA]);
						}

						if (p_flags & FLAG_FORCE_LINEAR) {
							color = color.srgb_to_linear();
						}

						*row_w++ = color.r;
						if (g_channel) {
							*row_w++ = color.g;
						}
						if (b_channel) {
							*row_w++ = color.b;
						}
						if (a_channel) {
							*row_w++ = color.a;
						}
					}
				}
			}
		}
	}

	p_image->set_data(exr_image.width, exr_image.height, false, format, imgdata);

	return OK;
}

void ImageLoaderTinyEXR::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("exr");
}

static Ref<Image> _tinyexr_mem_loader_func(const uint8_t *p_exr, int p_size) {
	Ref<FileAccessMemory> memfile;
	memfile.instantiate();
	Error open_memfile_error = memfile->open_custom(p_exr, p_size);
	ERR_FAIL_COND_V_MSG(open_memfile_error, Ref<Image>(), "Could not create memfile for EXR image buffer.");

	Ref<Image> img;
	img.instantiate();
	Error load_error = ImageLoaderTinyEXR().load_image(img, memfile, false, 1.0f);
	ERR_FAIL_COND_V_MSG(load_error, Ref<Image>(), "Failed to load EXR image.");
	return img;
}

ImageLoaderTinyEXR::ImageLoaderTinyEXR() {
	Image::_exr_mem_loader_func = _tinyexr_mem_loader_func;
}
