/**************************************************************************/
/*  packed_data_container_internal.h                                      */
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

#include "core/error/error_list.h"
#include "core/variant/variant.h"

#include <climits>
#include <cstdint>

namespace PackedDataContainerInternal {
inline Error checked_extent(uint64_t p_current, uint64_t p_extra, int64_t &r_end) {
	r_end = 0;
	if (p_current > UINT32_MAX || p_extra > UINT32_MAX - p_current) {
		return ERR_OUT_OF_MEMORY;
	}
	r_end = int64_t(p_current + p_extra);
	return OK;
}
inline Error checked_scalar_array_extent(uint64_t p_count, uint64_t p_stride) {
	const uint64_t maximum = uint64_t(INT_MAX) & ~uint64_t(3);
	return p_stride == 0 || p_count > (maximum - 8) / p_stride ? ERR_OUT_OF_MEMORY : OK;
}

// encode_variant's scalar length is an int. Check variable-sized values
// before asking it to measure or write them.
inline Error check_scalar(const Variant &p_value) {
	const uint64_t maximum = uint64_t(INT_MAX) & ~uint64_t(3);
	auto string_bytes = [](const String &p_string) {
		uint64_t bytes = 0;
		for (int i = 0; i < p_string.length(); i++) {
			const char32_t c = p_string[i];
			bytes += c <= 0x7f ? 1 : c <= 0x7ff ? 2
					: c <= 0xffff				? 3
												: 4;
		}
		return bytes;
	};
	auto add_string = [&](uint64_t &r_total, const String &p_string, bool p_terminated) {
		const uint64_t bytes = string_bytes(p_string) + (p_terminated ? 1 : 0);
		const uint64_t encoded = 4 + ((bytes + 3) & ~uint64_t(3));
		if (encoded > maximum || r_total > maximum - encoded) {
			return false;
		}
		r_total += encoded;
		return true;
	};
	switch (p_value.get_type()) {
		case Variant::STRING:
		case Variant::STRING_NAME: {
			uint64_t total = 4;
			return add_string(total, String(p_value), false) ? OK : ERR_OUT_OF_MEMORY;
		}
		case Variant::NODE_PATH: {
			const NodePath path = p_value;
			uint64_t total = 16;
			for (int i = 0; i < path.get_name_count(); i++) {
				if (!add_string(total, path.get_name(i), false)) {
					return ERR_OUT_OF_MEMORY;
				}
			}
			for (int i = 0; i < path.get_subname_count(); i++) {
				if (!add_string(total, path.get_subname(i), false)) {
					return ERR_OUT_OF_MEMORY;
				}
			}
			return OK;
		}
		case Variant::PACKED_STRING_ARRAY: {
			const Vector<String> strings = p_value;
			if (strings.size() > INT_MAX) {
				return ERR_OUT_OF_MEMORY;
			}
			uint64_t total = 8;
			for (const String &text : strings) {
				if (!add_string(total, text, true)) {
					return ERR_OUT_OF_MEMORY;
				}
			}
			return OK;
		}
		case Variant::PACKED_BYTE_ARRAY:
			return checked_scalar_array_extent(p_value.get_indexed_size(), 1);
		case Variant::PACKED_INT32_ARRAY:
		case Variant::PACKED_FLOAT32_ARRAY:
			return checked_scalar_array_extent(p_value.get_indexed_size(), 4);
		case Variant::PACKED_INT64_ARRAY:
		case Variant::PACKED_FLOAT64_ARRAY:
			return checked_scalar_array_extent(p_value.get_indexed_size(), 8);
		case Variant::PACKED_VECTOR2_ARRAY:
			return checked_scalar_array_extent(p_value.get_indexed_size(), sizeof(real_t) * 2);
		case Variant::PACKED_VECTOR3_ARRAY:
			return checked_scalar_array_extent(p_value.get_indexed_size(), sizeof(real_t) * 3);
		case Variant::PACKED_VECTOR4_ARRAY:
			return checked_scalar_array_extent(p_value.get_indexed_size(), sizeof(real_t) * 4);
		case Variant::PACKED_COLOR_ARRAY:
			return checked_scalar_array_extent(p_value.get_indexed_size(), 16);
		default:
			return OK;
	}
}
} // namespace PackedDataContainerInternal
