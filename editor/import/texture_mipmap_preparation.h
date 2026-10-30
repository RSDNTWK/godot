/**************************************************************************/
/*  texture_mipmap_preparation.h                                          */
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

#pragma once

#include "core/io/image.h"

#include <cstdint>
#include <utility>

namespace TextureMipmapPreparation {
// Finish the image preparation phase before the importer opens its output.
inline Error prepare_layers(const Vector<Ref<Image>> &p_images, Vector<Ref<Image>> &r_volume_mipmaps, bool p_volume, bool p_mipmaps, bool p_force_po2, bool p_renormalize) {
	if (p_images.is_empty()) {
		return ERR_INVALID_PARAMETER;
	}
	for (const Ref<Image> &image : p_images) {
		if (image.is_null() || image->is_empty()) {
			return ERR_INVALID_DATA;
		}
		if (p_volume) {
			image->clear_mipmaps();
			if (image->has_mipmaps()) {
				return ERR_OUT_OF_MEMORY;
			}
		}
		if (p_force_po2) {
			if (image->is_compressed()) {
				return ERR_UNAVAILABLE;
			}
			const Vector2i expected(Math::next_power_of_2(uint32_t(image->get_width())), Math::next_power_of_2(uint32_t(image->get_height())));
			image->resize_to_po2();
			if (image->get_size() != expected) {
				return ERR_OUT_OF_MEMORY;
			}
		}
		if (!p_volume) {
			if (p_mipmaps) {
				const Error error = image->generate_mipmaps(p_renormalize);
				if (error != OK) {
					return error;
				}
			} else {
				image->clear_mipmaps();
				if (image->has_mipmaps()) {
					return ERR_OUT_OF_MEMORY;
				}
			}
		}
	}
	if (!p_volume || !p_mipmaps) {
		r_volume_mipmaps.clear();
		return OK;
	}
	Vector<Ref<Image>> generated;
	Vector<Ref<Image>> parent_images = p_images;
	//create 3D mipmaps, this is horrible, though not used very often
	int w = p_images[0]->get_width();
	int h = p_images[0]->get_height();
	int d = p_images.size();

	while (w > 1 || h > 1 || d > 1) {
		Vector<Ref<Image>> mipmaps;
		int mm_w = MAX(1, w >> 1);
		int mm_h = MAX(1, h >> 1);
		int mm_d = MAX(1, d >> 1);
		const Error level_error = mipmaps.resize(mm_d);
		if (level_error != OK) {
			return level_error;
		}

		for (int i = 0; i < mm_d; i++) {
			Ref<Image> mm = Image::create_empty(mm_w, mm_h, false, p_images[0]->get_format());
			if (mm.is_null() || mm->is_empty()) {
				return ERR_OUT_OF_MEMORY;
			}
			Vector3 pos;
			pos.z = float(i) * float(d) / float(mm_d) + 0.5;
			for (int x = 0; x < mm_w; x++) {
				for (int y = 0; y < mm_h; y++) {
					pos.x = float(x) * float(w) / float(mm_w) + 0.5;
					pos.y = float(y) * float(h) / float(mm_h) + 0.5;

					Vector3i posi = Vector3i(pos);
					Vector3 fract = pos - Vector3(posi);
					Vector3i posi_n = posi;
					if (posi_n.x < w - 1) {
						posi_n.x++;
					}
					if (posi_n.y < h - 1) {
						posi_n.y++;
					}
					if (posi_n.z < d - 1) {
						posi_n.z++;
					}

					Color c000 = parent_images[posi.z]->get_pixel(posi.x, posi.y);
					Color c100 = parent_images[posi.z]->get_pixel(posi_n.x, posi.y);
					Color c010 = parent_images[posi.z]->get_pixel(posi.x, posi_n.y);
					Color c110 = parent_images[posi.z]->get_pixel(posi_n.x, posi_n.y);
					Color c001 = parent_images[posi_n.z]->get_pixel(posi.x, posi.y);
					Color c101 = parent_images[posi_n.z]->get_pixel(posi_n.x, posi.y);
					Color c011 = parent_images[posi_n.z]->get_pixel(posi.x, posi_n.y);
					Color c111 = parent_images[posi_n.z]->get_pixel(posi_n.x, posi_n.y);

					Color cx00 = c000.lerp(c100, fract.x);
					Color cx01 = c001.lerp(c101, fract.x);
					Color cx10 = c010.lerp(c110, fract.x);
					Color cx11 = c011.lerp(c111, fract.x);

					Color cy0 = cx00.lerp(cx10, fract.y);
					Color cy1 = cx01.lerp(cx11, fract.y);

					Color cz = cy0.lerp(cy1, fract.z);

					mm->set_pixel(x, y, cz);
				}
			}

			mipmaps.write[i] = mm;
		}

		w = mm_w;
		h = mm_h;
		d = mm_d;

		const int64_t offset = generated.size();
		if (mipmaps.size() > INT64_MAX - offset) {
			return ERR_OUT_OF_MEMORY;
		}
		const Error output_error = generated.resize(offset + mipmaps.size());
		if (output_error != OK) {
			return output_error;
		}
		for (int64_t i = 0; i < mipmaps.size(); i++) {
			generated.write[offset + i] = mipmaps[i];
		}
		parent_images = std::move(mipmaps);
	}
	r_volume_mipmaps = std::move(generated);
	return OK;
}
} // namespace TextureMipmapPreparation
