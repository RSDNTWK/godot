/**************************************************************************/
/*  image_loader_bmp.cpp                                                  */
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

#include "image_loader_bmp.h"

#include "core/io/file_access_memory.h"
#include "core/io/image_loader_safety.h"
#include "core/io/marshalls.h"

static uint8_t get_mask_width(uint16_t mask) {
	// Returns number of ones in the binary value of the parameter: mask.
	// Uses a Simple pop_count.
	uint8_t c = 0u;
	for (; mask != 0u; mask &= mask - 1u) {
		c++;
	}
	return c;
}

Error ImageLoaderBMP::convert_to_image(Ref<Image> p_image,
		const uint8_t *p_buffer,
		const uint8_t *p_color_buffer,
		const uint32_t color_table_size,
		const bmp_header_s &p_header) {
	Error err = OK;

	if (p_buffer == nullptr) {
		err = FAILED;
	}

	if (err == OK) {
		size_t index = 0;
		size_t width = (size_t)p_header.bmp_info_header.bmp_width;
		size_t height = (size_t)p_header.bmp_info_header.bmp_height;
		size_t bits_per_pixel = (size_t)p_header.bmp_info_header.bmp_bit_count;

		// Image data (might be indexed)
		Vector<uint8_t> data;
		int64_t data_len = 0;
		err = ImageLoaderSafety::image_bytes(width, height, bits_per_pixel <= 8 ? 1 : 4, data_len);
		ERR_FAIL_COND_V(err != OK, err);
		const uint64_t width_bytes = (uint64_t(width) * bits_per_pixel + 7) / 8;
		const uint64_t line_width = (width_bytes + 3) & ~uint64_t(3);
		ERR_FAIL_COND_V(p_header.bmp_file_header.bmp_file_offset > p_header.bmp_file_header.bmp_file_size, ERR_FILE_CORRUPT);
		const uint64_t available = uint64_t(p_header.bmp_file_header.bmp_file_size) - p_header.bmp_file_header.bmp_file_offset;
		ERR_FAIL_COND_V(line_width * height > available, ERR_FILE_CORRUPT);
		const uint8_t *line = p_buffer + line_width * (height - 1);

		if (bits_per_pixel <= 8) { // indexed
			data_len = int64_t(width) * height;
		} else { // color
			data_len = int64_t(width) * height * 4;
		}
		ERR_FAIL_COND_V_MSG(data_len == 0, ERR_BUG, "Couldn't parse the BMP image data.");
		err = data.resize(data_len);
		ERR_FAIL_COND_V(err != OK, err);

		uint8_t *data_w = data.ptrw();
		uint8_t *write_buffer = data_w;

		for (uint64_t i = 0; i < height; i++) {
			const uint8_t *line_ptr = line;

			for (unsigned int j = 0; j < width; j++) {
				switch (bits_per_pixel) {
					case 1: {
						write_buffer[index] = (line[(j * bits_per_pixel) / 8] >> (8 - bits_per_pixel * (1 + j % 8))) & 0x01;

						index++;
					} break;
					case 2: {
						write_buffer[index] = (line[(j * bits_per_pixel) / 8] >> (8 - bits_per_pixel * (1 + j % 4))) & 0x03;

						index++;
					} break;
					case 4: {
						write_buffer[index] = (line[(j * bits_per_pixel) / 8] >> (8 - bits_per_pixel * (1 + j % 2))) & 0x0f;

						index++;
					} break;
					case 8: {
						uint8_t color_index = *line_ptr;

						write_buffer[index] = color_index;

						index += 1;
						line_ptr += 1;
					} break;
					case 16: {
						uint16_t rgb = (static_cast<uint16_t>(line_ptr[1]) << 8) | line_ptr[0];
						// A1R5G5B5/X1R5G5B5 => uint16_t
						// [A/X]1R5G2 | G3B5 => uint8_t | uint8_t
						uint8_t ba = (rgb & p_header.bmp_bitfield.alpha_mask) >> p_header.bmp_bitfield.alpha_offset; // Alpha 0b 1000 ...
						uint8_t b0 = (rgb & p_header.bmp_bitfield.red_mask) >> p_header.bmp_bitfield.red_offset; // Red   0b 0111 1100 ...
						uint8_t b1 = (rgb & p_header.bmp_bitfield.green_mask) >> p_header.bmp_bitfield.green_offset; // Green 0b 0000 0011 1110 ...
						uint8_t b2 = (rgb & p_header.bmp_bitfield.blue_mask); // >> p_header.bmp_bitfield.blue_offset; // Blue  0b  ... 0001 1111

						// Next we apply some color scaling going from a variable value space to a 256 value space.
						// This may be simplified some but left as is for legibility.
						// float scaled_value = unscaled_value * byte_max_value / color_channel_maximum_value + rounding_offset;
						float f0 = b0 * 255.0f / static_cast<float>(p_header.bmp_bitfield.red_max) + 0.5f;
						float f1 = b1 * 255.0f / static_cast<float>(p_header.bmp_bitfield.green_max) + 0.5f;
						float f2 = b2 * 255.0f / static_cast<float>(p_header.bmp_bitfield.blue_max) + 0.5f;
						write_buffer[index + 0] = static_cast<uint8_t>(f0); // R
						write_buffer[index + 1] = static_cast<uint8_t>(f1); // G
						write_buffer[index + 2] = static_cast<uint8_t>(f2); // B

						if (p_header.bmp_bitfield.alpha_mask_width > 0) {
							write_buffer[index + 3] = ba * 0xFF; // Alpha value(Always true or false so no scaling)
						} else {
							write_buffer[index + 3] = 0xFF; // No Alpha channel, Show everything.
						}

						index += 4;
						line_ptr += 2;
					} break;
					case 24: {
						write_buffer[index + 2] = line_ptr[0];
						write_buffer[index + 1] = line_ptr[1];
						write_buffer[index + 0] = line_ptr[2];
						write_buffer[index + 3] = 0xff;

						index += 4;
						line_ptr += 3;
					} break;
					case 32: {
						write_buffer[index + 2] = line_ptr[0];
						write_buffer[index + 1] = line_ptr[1];
						write_buffer[index + 0] = line_ptr[2];
						write_buffer[index + 3] = line_ptr[3];

						index += 4;
						line_ptr += 4;
					} break;
				}
			}
			if (i + 1 < height) {
				line -= line_width;
			}
		}

		if (p_color_buffer == nullptr || color_table_size == 0) { // regular pixels

			p_image->set_data(width, height, false, Image::FORMAT_RGBA8, data);

		} else { // data is in indexed format, extend it

			// Palette data
			Vector<uint8_t> palette_data;
			err = palette_data.resize(uint64_t(color_table_size) * 4);
			ERR_FAIL_COND_V(err != OK, err);

			uint8_t *palette_data_w = palette_data.ptrw();
			uint8_t *pal = palette_data_w;

			const uint8_t *cb = p_color_buffer;

			for (unsigned int i = 0; i < color_table_size; ++i) {
				pal[i * 4 + 0] = cb[2];
				pal[i * 4 + 1] = cb[1];
				pal[i * 4 + 2] = cb[0];
				pal[i * 4 + 3] = 0xff;

				cb += 4;
			}
			// Extend palette to image
			Vector<uint8_t> extended_data;
			err = extended_data.resize(data.size() * 4);
			ERR_FAIL_COND_V(err != OK, err);

			uint8_t *ex_w = extended_data.ptrw();
			uint8_t *dest = ex_w;

			const int num_pixels = width * height;

			for (int i = 0; i < num_pixels; i++) {
				ERR_FAIL_COND_V(write_buffer[i] >= color_table_size, ERR_FILE_CORRUPT);
				dest[0] = pal[write_buffer[i] * 4 + 0];
				dest[1] = pal[write_buffer[i] * 4 + 1];
				dest[2] = pal[write_buffer[i] * 4 + 2];
				dest[3] = pal[write_buffer[i] * 4 + 3];

				dest += 4;
			}
			p_image->set_data(width, height, false, Image::FORMAT_RGBA8, extended_data);
		}
	}
	return err;
}

Error ImageLoaderBMP::load_image(Ref<Image> p_image, Ref<FileAccess> f, BitField<ImageFormatLoader::LoaderFlags> p_flags, float p_scale) {
	bmp_header_s bmp_header;
	Error err = ERR_INVALID_DATA;
	const uint64_t start = f->get_position();
	const uint64_t file_length = f->get_length();
	ERR_FAIL_COND_V(start > file_length, ERR_FILE_CORRUPT);
	const uint64_t available = file_length - start;

	// A valid bmp file should always at least have a
	// file header and a minimal info header
	if (available >= BITMAP_FILE_HEADER_SIZE + BITMAP_INFO_HEADER_MIN_SIZE) {
		// File Header
		uint8_t header[BITMAP_FILE_HEADER_SIZE + BITMAP_INFO_HEADER_MIN_SIZE];
		err = ImageLoaderSafety::read_exact(f, header, sizeof(header));
		ERR_FAIL_COND_V(err != OK, err);
		bmp_header.bmp_file_header.bmp_signature = decode_uint16(header);
		ERR_FAIL_COND_V(bmp_header.bmp_file_header.bmp_signature != BITMAP_SIGNATURE, ERR_INVALID_DATA);
		if (bmp_header.bmp_file_header.bmp_signature == BITMAP_SIGNATURE) {
			bmp_header.bmp_file_header.bmp_file_size = decode_uint32(header + 2);
			bmp_header.bmp_file_header.bmp_file_padding = decode_uint32(header + 6);
			bmp_header.bmp_file_header.bmp_file_offset = decode_uint32(header + 10);

			// Info Header
			bmp_header.bmp_info_header.bmp_header_size = decode_uint32(header + 14);
			ERR_FAIL_COND_V_MSG(bmp_header.bmp_info_header.bmp_header_size < BITMAP_INFO_HEADER_MIN_SIZE, ERR_FILE_CORRUPT,
					vformat("Couldn't parse the BMP info header. The file is likely corrupt: %s", f->get_path()));

			bmp_header.bmp_info_header.bmp_width = decode_uint32(header + 18);
			bmp_header.bmp_info_header.bmp_height = decode_uint32(header + 22);

			bmp_header.bmp_info_header.bmp_planes = decode_uint16(header + 26);
			ERR_FAIL_COND_V_MSG(bmp_header.bmp_info_header.bmp_planes != 1, ERR_FILE_CORRUPT,
					vformat("Couldn't parse the BMP planes. The file is likely corrupt: %s", f->get_path()));

			bmp_header.bmp_info_header.bmp_bit_count = decode_uint16(header + 28);
			bmp_header.bmp_info_header.bmp_compression = decode_uint32(header + 30);
			bmp_header.bmp_info_header.bmp_size_image = decode_uint32(header + 34);
			bmp_header.bmp_info_header.bmp_pixels_per_meter_x = decode_uint32(header + 38);
			bmp_header.bmp_info_header.bmp_pixels_per_meter_y = decode_uint32(header + 42);
			bmp_header.bmp_info_header.bmp_colors_used = decode_uint32(header + 46);
			bmp_header.bmp_info_header.bmp_important_colors = decode_uint32(header + 50);
			ERR_FAIL_COND_V(f->get_position() != start + BITMAP_FILE_HEADER_SIZE + BITMAP_INFO_HEADER_MIN_SIZE, ERR_FILE_CORRUPT);
			ERR_FAIL_COND_V(f->get_error() != OK && f->get_error() != ERR_FILE_EOF, f->get_error());
			ERR_FAIL_COND_V(bmp_header.bmp_info_header.bmp_header_size > available - BITMAP_FILE_HEADER_SIZE, ERR_FILE_CORRUPT);
			ERR_FAIL_COND_V(bmp_header.bmp_file_header.bmp_file_size > available || bmp_header.bmp_file_header.bmp_file_offset > bmp_header.bmp_file_header.bmp_file_size, ERR_FILE_CORRUPT);
			const uint16_t depth = bmp_header.bmp_info_header.bmp_bit_count;
			ERR_FAIL_COND_V(depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16 && depth != 24 && depth != 32, ERR_FILE_CORRUPT);
			int64_t decoded_bytes = 0;
			err = ImageLoaderSafety::image_bytes(bmp_header.bmp_info_header.bmp_width, bmp_header.bmp_info_header.bmp_height, 4, decoded_bytes);
			ERR_FAIL_COND_V(err != OK, err);

			switch (bmp_header.bmp_info_header.bmp_compression) {
				case BI_ALPHABITFIELDS:
				case BI_BITFIELDS: {
					ERR_FAIL_COND_V(depth != 16 && depth != 32, ERR_FILE_CORRUPT);
					ERR_FAIL_COND_V(bmp_header.bmp_file_header.bmp_file_offset < BITMAP_FILE_HEADER_SIZE + BITMAP_INFO_HEADER_MIN_SIZE + 12, ERR_FILE_CORRUPT);
					uint8_t masks[16];
					const bool has_alpha = bmp_header.bmp_file_header.bmp_file_offset >= BITMAP_FILE_HEADER_SIZE + BITMAP_INFO_HEADER_MIN_SIZE + 16;
					err = ImageLoaderSafety::read_exact(f, masks, has_alpha ? 16 : 12);
					ERR_FAIL_COND_V(err != OK, err);
					bmp_header.bmp_bitfield.red_mask = decode_uint32(masks);
					bmp_header.bmp_bitfield.green_mask = decode_uint32(masks + 4);
					bmp_header.bmp_bitfield.blue_mask = decode_uint32(masks + 8);
					bmp_header.bmp_bitfield.alpha_mask = 0;
					if (bmp_header.bmp_file_header.bmp_file_offset >= BITMAP_FILE_HEADER_SIZE + BITMAP_INFO_HEADER_MIN_SIZE + 16) {
						bmp_header.bmp_bitfield.alpha_mask = decode_uint32(masks + 12);
					} else {
						ERR_FAIL_COND_V(bmp_header.bmp_info_header.bmp_compression == BI_ALPHABITFIELDS, ERR_FILE_CORRUPT);
					}
					ERR_FAIL_COND_V(f->get_error() != OK && f->get_error() != ERR_FILE_EOF, f->get_error());
					if (depth == 16) {
						const uint16_t r = bmp_header.bmp_bitfield.red_mask;
						const uint16_t g = bmp_header.bmp_bitfield.green_mask;
						const uint16_t b = bmp_header.bmp_bitfield.blue_mask;
						const uint16_t a = bmp_header.bmp_bitfield.alpha_mask;
						ERR_FAIL_COND_V(!r || !g || !b || (r & g) || (r & b) || (g & b) || (a & (r | g | b)), ERR_FILE_CORRUPT);
					}

					bmp_header.bmp_bitfield.red_mask_width = get_mask_width(bmp_header.bmp_bitfield.red_mask);
					bmp_header.bmp_bitfield.green_mask_width = get_mask_width(bmp_header.bmp_bitfield.green_mask);
					bmp_header.bmp_bitfield.blue_mask_width = get_mask_width(bmp_header.bmp_bitfield.blue_mask);
					bmp_header.bmp_bitfield.alpha_mask_width = get_mask_width(bmp_header.bmp_bitfield.alpha_mask);

					bmp_header.bmp_bitfield.alpha_offset = bmp_header.bmp_bitfield.red_mask_width + bmp_header.bmp_bitfield.green_mask_width + bmp_header.bmp_bitfield.blue_mask_width;
					bmp_header.bmp_bitfield.red_offset = bmp_header.bmp_bitfield.green_mask_width + bmp_header.bmp_bitfield.blue_mask_width;
					bmp_header.bmp_bitfield.green_offset = bmp_header.bmp_bitfield.blue_mask_width;

					bmp_header.bmp_bitfield.red_max = (1 << bmp_header.bmp_bitfield.red_mask_width) - 1;
					bmp_header.bmp_bitfield.green_max = (1 << bmp_header.bmp_bitfield.green_mask_width) - 1;
					bmp_header.bmp_bitfield.blue_max = (1 << bmp_header.bmp_bitfield.blue_mask_width) - 1;
				} break;
				case BI_RLE8:
				case BI_RLE4:
				case BI_CMYKRLE8:
				case BI_CMYKRLE4: {
					// Stop parsing.
					ERR_FAIL_V_MSG(ERR_UNAVAILABLE,
							vformat("RLE compressed BMP files are not yet supported: %s", f->get_path()));
				} break;
				case BI_RGB:
					break;
				default:
					return ERR_UNAVAILABLE;
			}
			// Don't rely on sizeof(bmp_file_header) as structure padding
			// adds 2 bytes offset leading to misaligned color table reading
			uint64_t ct_offset = uint64_t(BITMAP_FILE_HEADER_SIZE) + bmp_header.bmp_info_header.bmp_header_size;
			ERR_FAIL_COND_V(ct_offset > bmp_header.bmp_file_header.bmp_file_offset, ERR_FILE_CORRUPT);
			f->seek(start + ct_offset);
			ERR_FAIL_COND_V(f->get_position() != start + ct_offset, ERR_FILE_CANT_READ);

			uint32_t color_table_size = 0;

			// bmp_colors_used may report 0 despite having a color table
			// for 4 and 1 bit images, so don't rely on this information
			if (bmp_header.bmp_info_header.bmp_bit_count <= 8) {
				// Support 256 colors max
				color_table_size = 1 << bmp_header.bmp_info_header.bmp_bit_count;
				ERR_FAIL_COND_V_MSG(color_table_size == 0, ERR_BUG,
						vformat("Couldn't parse the BMP color table: %s", f->get_path()));
				if (bmp_header.bmp_info_header.bmp_colors_used != 0) {
					ERR_FAIL_COND_V(bmp_header.bmp_info_header.bmp_colors_used > color_table_size, ERR_FILE_CORRUPT);
					color_table_size = bmp_header.bmp_info_header.bmp_colors_used;
				}
			}

			Vector<uint8_t> bmp_color_table;
			// Color table is usually 4 bytes per color -> [B][G][R][0]
			ERR_FAIL_COND_V(uint64_t(color_table_size) * 4 > bmp_header.bmp_file_header.bmp_file_offset - ct_offset, ERR_FILE_CORRUPT);
			err = bmp_color_table.resize(uint64_t(color_table_size) * 4);
			ERR_FAIL_COND_V(err != OK, err);
			uint8_t *bmp_color_table_w = bmp_color_table.ptrw();
			err = ImageLoaderSafety::read_exact(f, bmp_color_table_w, uint64_t(color_table_size) * 4);
			ERR_FAIL_COND_V(err != OK, err);

			f->seek(start + bmp_header.bmp_file_header.bmp_file_offset);
			ERR_FAIL_COND_V(f->get_position() != start + bmp_header.bmp_file_header.bmp_file_offset, ERR_FILE_CANT_READ);

			uint32_t bmp_buffer_size = (bmp_header.bmp_file_header.bmp_file_size - bmp_header.bmp_file_header.bmp_file_offset);

			Vector<uint8_t> bmp_buffer;
			err = bmp_buffer.resize(bmp_buffer_size);
			if (err == OK) {
				uint8_t *bmp_buffer_w = bmp_buffer.ptrw();
				err = ImageLoaderSafety::read_exact(f, bmp_buffer_w, bmp_buffer_size);
				ERR_FAIL_COND_V(err != OK, err);

				const uint8_t *bmp_buffer_r = bmp_buffer.ptr();
				const uint8_t *bmp_color_table_r = bmp_color_table.ptr();
				err = convert_to_image(p_image, bmp_buffer_r,
						bmp_color_table_r, color_table_size, bmp_header);
			}
		}
	}
	return err;
}

void ImageLoaderBMP::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("bmp");
}

static Ref<Image> _bmp_mem_loader_func(const uint8_t *p_bmp, int p_size) {
	Ref<FileAccessMemory> memfile;
	memfile.instantiate();
	Error open_memfile_error = memfile->open_custom(p_bmp, p_size);
	ERR_FAIL_COND_V_MSG(open_memfile_error, Ref<Image>(), "Could not create memfile for BMP image buffer.");

	Ref<Image> img;
	img.instantiate();
	Error load_error = ImageLoaderBMP().load_image(img, memfile, false, 1.0f);
	ERR_FAIL_COND_V_MSG(load_error, Ref<Image>(), "Failed to load BMP image.");
	return img;
}

ImageLoaderBMP::ImageLoaderBMP() {
	Image::_bmp_mem_loader_func = _bmp_mem_loader_func;
}
