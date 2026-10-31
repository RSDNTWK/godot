/**************************************************************************/
/*  packed_data_container.cpp                                             */
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

#include "packed_data_container.h"

#include "packed_data_container_internal.h"

#include "core/io/marshalls.h"
#include "core/object/class_db.h"

#include <climits>

Variant PackedDataContainer::getvar(const Variant &p_key, bool *r_valid) const {
	bool err = false;
	Variant ret = _key_at_ofs(0, p_key, err);
	if (r_valid) {
		*r_valid = !err;
	}
	if (err) {
		return Object::getvar(p_key, r_valid);
	}
	return ret;
}

int PackedDataContainer::size() const {
	return _size(0);
}

Variant PackedDataContainer::_iter_init_ofs(const Array &p_iter, uint32_t p_offset) {
	Array ref = p_iter;
	int size = _size(p_offset);
	if (size <= 0 || ref.size() != 1) {
		return false;
	} else {
		ref[0] = 0;
		return true;
	}
}

Variant PackedDataContainer::_iter_next_ofs(const Array &p_iter, uint32_t p_offset) {
	Array ref = p_iter;
	int size = _size(p_offset);
	if (ref.size() != 1) {
		return false;
	}
	int pos = ref[0];
	if (pos < 0 || pos >= size) {
		return false;
	}
	pos += 1;
	ref[0] = pos;
	return pos != size;
}

Variant PackedDataContainer::_iter_get_ofs(const Variant &p_iter, uint32_t p_offset) {
	int size = _size(p_offset);
	int pos = p_iter;
	if (pos < 0 || pos >= size) {
		return Variant();
	}

	const uint8_t *rd = data.ptr();
	const uint8_t *r = &rd[p_offset];
	uint32_t type = decode_uint32(r);

	bool err = false;
	if (type == TYPE_ARRAY) {
		uint32_t vpos = decode_uint32(rd + uint64_t(p_offset) + 8 + uint64_t(pos) * 4);
		return _get_at_ofs(vpos, rd, err);

	} else if (type == TYPE_DICT) {
		uint32_t vpos = decode_uint32(rd + uint64_t(p_offset) + 8 + uint64_t(pos) * 12 + 4);
		return _get_at_ofs(vpos, rd, err);
	} else {
		ERR_FAIL_V(Variant());
	}
}

Variant PackedDataContainer::_get_at_ofs(uint32_t p_ofs, const uint8_t *p_buf, bool &err) const {
	err = true;
	const int64_t remaining = data.size() - int64_t(p_ofs);
	ERR_FAIL_COND_V(remaining < 4, Variant());
	uint32_t type = decode_uint32(p_buf + p_ofs);

	if (type == TYPE_ARRAY || type == TYPE_DICT) {
		uint32_t container_type;
		uint32_t count;
		ERR_FAIL_COND_V(!_read_container_header(p_ofs, container_type, count), Variant());
		Ref<PackedDataContainerRef> pdcr = memnew(PackedDataContainerRef);
		Ref<PackedDataContainer> pdc = Ref<PackedDataContainer>(const_cast<PackedDataContainer *>(this));

		pdcr->from = pdc;
		pdcr->offset = p_ofs;
		err = false;
		return pdcr;
	} else {
		Variant v;
		Error rerr = decode_variant(v, p_buf + p_ofs, int(MIN(remaining, int64_t(INT_MAX))), nullptr, false);

		if (rerr != OK) {
			err = true;
			ERR_FAIL_COND_V_MSG(err != OK, Variant(), "Error when trying to decode Variant.");
		}
		err = false;
		return v;
	}
}

uint32_t PackedDataContainer::_type_at_ofs(uint32_t p_ofs) const {
	ERR_FAIL_COND_V(data.size() - int64_t(p_ofs) < 4, 0);
	const uint8_t *rd = data.ptr();
	ERR_FAIL_NULL_V(rd, 0);
	const uint8_t *r = &rd[p_ofs];
	uint32_t type = decode_uint32(r);

	return type;
}

bool PackedDataContainer::_read_container_header(uint32_t p_ofs, uint32_t &r_type, uint32_t &r_count) const {
	const int64_t remaining = data.size() - int64_t(p_ofs);
	if (remaining < 8) {
		return false;
	}
	const uint8_t *header = data.ptr() + p_ofs;
	r_type = decode_uint32(header);
	if (r_type != TYPE_ARRAY && r_type != TYPE_DICT) {
		return false;
	}
	r_count = decode_uint32(header + 4);
	const int stride = r_type == TYPE_ARRAY ? 4 : 12;
	// Check the immediate table on every access: nested refs retain a live owner.
	// Children are checked lazily, preserving shared and backward offsets.
	return r_count <= uint32_t(INT_MAX) && int64_t(r_count) <= (remaining - 8) / stride;
}

int PackedDataContainer::_size(uint32_t p_ofs) const {
	uint32_t type;
	uint32_t count;
	ERR_FAIL_COND_V(!_read_container_header(p_ofs, type, count), 0);
	return int(count);
}

Variant PackedDataContainer::_key_at_ofs(uint32_t p_ofs, const Variant &p_key, bool &err) const {
	err = true;
	uint32_t type;
	uint32_t count;
	ERR_FAIL_COND_V(!_read_container_header(p_ofs, type, count), Variant());
	const uint8_t *rd = data.ptr();
	if (!rd) {
		err = true;
		ERR_FAIL_NULL_V(rd, Variant());
	}
	const uint8_t *r = &rd[p_ofs];

	if (type == TYPE_ARRAY) {
		if (p_key.is_num()) {
			int idx = p_key;
			int len = int(count);
			if (idx < 0 || idx >= len) {
				err = true;
				return Variant();
			}
			uint32_t ofs = decode_uint32(r + 8 + uint64_t(idx) * 4);
			return _get_at_ofs(ofs, rd, err);

		} else {
			err = true;
			return Variant();
		}

	} else if (type == TYPE_DICT) {
		uint32_t hash = p_key.hash();
		uint32_t len = count;

		bool found = false;
		for (uint32_t i = 0; i < len; i++) {
			uint32_t khash = decode_uint32(r + 8 + uint64_t(i) * 12 + 0);
			if (khash == hash) {
				Variant key = _get_at_ofs(decode_uint32(r + 8 + uint64_t(i) * 12 + 4), rd, err);
				if (err) {
					return Variant();
				}
				if (key == p_key) {
					//key matches, return value
					return _get_at_ofs(decode_uint32(r + 8 + uint64_t(i) * 12 + 8), rd, err);
				}
				found = true;
			} else {
				if (found) {
					break;
				}
			}
		}

		err = true;
		return Variant();

	} else {
		err = true;
		return Variant();
	}
}

Error PackedDataContainer::_pack(const Variant &p_data, Vector<uint8_t> &tmpdata, HashMap<String, uint32_t> &string_cache, uint32_t &r_offset, const void **p_active, int p_depth) {
	ERR_FAIL_COND_V_MSG(p_depth > Variant::MAX_RECURSION_DEPTH, ERR_OUT_OF_MEMORY, "PackedDataContainer nesting is too deep.");
	const uint64_t position = tmpdata.size();
	ERR_FAIL_COND_V(position > UINT32_MAX, ERR_OUT_OF_MEMORY);
	r_offset = uint32_t(position);

	const Variant::Type type = p_data.get_type();
	if (type == Variant::ARRAY || type == Variant::DICTIONARY) {
		const void *identity = type == Variant::ARRAY ? Array(p_data).id() : Dictionary(p_data).id();
		for (int i = 0; i < p_depth; i++) {
			ERR_FAIL_COND_V_MSG(p_active[i] == identity, ERR_INVALID_DATA, "Cannot pack a cyclic container.");
		}
		p_active[p_depth] = identity;
	}

	auto grow = [&](uint64_t p_bytes) {
		int64_t end = 0;
		const Error error = PackedDataContainerInternal::checked_extent(tmpdata.size(), p_bytes, end);
		return error == OK ? tmpdata.resize(end) : error;
	};
	switch (type) {
		case Variant::STRING: {
			String s = p_data;
			if (string_cache.has(s)) {
				r_offset = string_cache[s];
				return OK;
			}
			string_cache[s] = r_offset;
			[[fallthrough]];
		}
		case Variant::NIL:
		case Variant::BOOL:
		case Variant::INT:
		case Variant::FLOAT:
		case Variant::VECTOR2:
		case Variant::VECTOR2I:
		case Variant::RECT2:
		case Variant::RECT2I:
		case Variant::VECTOR3:
		case Variant::VECTOR3I:
		case Variant::VECTOR4:
		case Variant::VECTOR4I:
		case Variant::TRANSFORM2D:
		case Variant::PLANE:
		case Variant::QUATERNION:
		case Variant::AABB:
		case Variant::BASIS:
		case Variant::TRANSFORM3D:
		case Variant::PROJECTION:
		case Variant::COLOR:
		case Variant::PACKED_BYTE_ARRAY:
		case Variant::PACKED_INT32_ARRAY:
		case Variant::PACKED_INT64_ARRAY:
		case Variant::PACKED_FLOAT32_ARRAY:
		case Variant::PACKED_FLOAT64_ARRAY:
		case Variant::PACKED_STRING_ARRAY:
		case Variant::PACKED_VECTOR2_ARRAY:
		case Variant::PACKED_VECTOR3_ARRAY:
		case Variant::PACKED_COLOR_ARRAY:
		case Variant::PACKED_VECTOR4_ARRAY:
		case Variant::STRING_NAME:
		case Variant::NODE_PATH: {
			int len = 0;
			Error error = PackedDataContainerInternal::check_scalar(p_data);
			ERR_FAIL_COND_V(error != OK, error);
			error = encode_variant(p_data, nullptr, len, false);
			ERR_FAIL_COND_V(error != OK, error);
			ERR_FAIL_COND_V(len < 0, ERR_INVALID_DATA);
			error = grow(uint64_t(len));
			ERR_FAIL_COND_V(error != OK, error);
			int written = 0;
			error = encode_variant(p_data, tmpdata.ptrw() + position, written, false, 0, len);
			ERR_FAIL_COND_V(error != OK, error);
			ERR_FAIL_COND_V(written != len, ERR_INVALID_DATA);
			return OK;
		}
		case Variant::RID:
		case Variant::OBJECT: {
			// Preserve the established serialization of object handles as NIL.
			return _pack(Variant(), tmpdata, string_cache, r_offset, p_active, p_depth);
		}
		case Variant::DICTIONARY: {
			const Dictionary d = p_data;
			const int len = d.size();
			Error error = grow(uint64_t(len) * 12 + 8);
			ERR_FAIL_COND_V(error != OK, error);
			encode_uint32(TYPE_DICT, tmpdata.ptrw() + position);
			encode_uint32(len, tmpdata.ptrw() + position + 4);
			List<DictKey> sortk;
			for (const KeyValue<Variant, Variant> &kv : d) {
				DictKey dk;
				dk.hash = kv.key.hash();
				dk.key = kv.key;
				sortk.push_back(dk);
			}
			sortk.sort();
			int idx = 0;
			for (const DictKey &entry : sortk) {
				const uint64_t slot = position + 8 + uint64_t(idx) * 12;
				encode_uint32(entry.hash, tmpdata.ptrw() + slot);
				uint32_t offset = 0;
				error = _pack(entry.key, tmpdata, string_cache, offset, p_active, p_depth + 1);
				ERR_FAIL_COND_V(error != OK, error);
				encode_uint32(offset, tmpdata.ptrw() + slot + 4);
				error = _pack(d[entry.key], tmpdata, string_cache, offset, p_active, p_depth + 1);
				ERR_FAIL_COND_V(error != OK, error);
				encode_uint32(offset, tmpdata.ptrw() + slot + 8);
				idx++;
			}
			return OK;
		}
		case Variant::ARRAY: {
			const Array a = p_data;
			const int len = a.size();
			Error error = grow(uint64_t(len) * 4 + 8);
			ERR_FAIL_COND_V(error != OK, error);
			encode_uint32(TYPE_ARRAY, tmpdata.ptrw() + position);
			encode_uint32(len, tmpdata.ptrw() + position + 4);
			for (int i = 0; i < len; i++) {
				uint32_t offset = 0;
				error = _pack(a[i], tmpdata, string_cache, offset, p_active, p_depth + 1);
				ERR_FAIL_COND_V(error != OK, error);
				encode_uint32(offset, tmpdata.ptrw() + position + 8 + uint64_t(i) * 4);
			}
			return OK;
		}
		default:
			ERR_FAIL_V_MSG(ERR_INVALID_DATA, "Unsupported value in PackedDataContainer.");
	}
}

Error PackedDataContainer::pack(const Variant &p_data) {
	ERR_FAIL_COND_V_MSG(p_data.get_type() != Variant::ARRAY && p_data.get_type() != Variant::DICTIONARY, ERR_INVALID_DATA, "PackedDataContainer can pack only Array and Dictionary type.");
	Vector<uint8_t> tmpdata;
	HashMap<String, uint32_t> string_cache;
	const void *active[Variant::MAX_RECURSION_DEPTH + 1] = {};
	uint32_t offset = 0;
	const Error error = _pack(p_data, tmpdata, string_cache, offset, active, 0);
	ERR_FAIL_COND_V(error != OK, error);
	// Publish only the complete serialization, without another fallible copy.
	data = std::move(tmpdata);
	return OK;
}

void PackedDataContainer::_set_data(const Vector<uint8_t> &p_data) {
	data = p_data;
}

Vector<uint8_t> PackedDataContainer::_get_data() const {
	return data;
}

Variant PackedDataContainer::_iter_init(const Array &p_iter) {
	return _iter_init_ofs(p_iter, 0);
}

Variant PackedDataContainer::_iter_next(const Array &p_iter) {
	return _iter_next_ofs(p_iter, 0);
}

Variant PackedDataContainer::_iter_get(const Variant &p_iter) {
	return _iter_get_ofs(p_iter, 0);
}

void PackedDataContainer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_set_data", "data"), &PackedDataContainer::_set_data);
	ClassDB::bind_method(D_METHOD("_get_data"), &PackedDataContainer::_get_data);
	ClassDB::bind_method(D_METHOD("_iter_init"), &PackedDataContainer::_iter_init);
	ClassDB::bind_method(D_METHOD("_iter_get"), &PackedDataContainer::_iter_get);
	ClassDB::bind_method(D_METHOD("_iter_next"), &PackedDataContainer::_iter_next);
	ClassDB::bind_method(D_METHOD("pack", "value"), &PackedDataContainer::pack);
	ClassDB::bind_method(D_METHOD("size"), &PackedDataContainer::size);

	BIND_METHOD_ERR_RETURN_DOC("pack", ERR_INVALID_DATA, ERR_OUT_OF_MEMORY);

	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "__data__", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_INTERNAL), "_set_data", "_get_data");
}

//////////////////

Variant PackedDataContainerRef::_iter_init(const Array &p_iter) {
	return from->_iter_init_ofs(p_iter, offset);
}

Variant PackedDataContainerRef::_iter_next(const Array &p_iter) {
	return from->_iter_next_ofs(p_iter, offset);
}

Variant PackedDataContainerRef::_iter_get(const Variant &p_iter) {
	return from->_iter_get_ofs(p_iter, offset);
}

void PackedDataContainerRef::_bind_methods() {
	ClassDB::bind_method(D_METHOD("size"), &PackedDataContainerRef::size);
	ClassDB::bind_method(D_METHOD("_iter_init"), &PackedDataContainerRef::_iter_init);
	ClassDB::bind_method(D_METHOD("_iter_get"), &PackedDataContainerRef::_iter_get);
	ClassDB::bind_method(D_METHOD("_iter_next"), &PackedDataContainerRef::_iter_next);
}

Variant PackedDataContainerRef::getvar(const Variant &p_key, bool *r_valid) const {
	bool err = false;
	Variant ret = from->_key_at_ofs(offset, p_key, err);
	if (r_valid) {
		*r_valid = !err;
	}
	return ret;
}

int PackedDataContainerRef::size() const {
	return from->_size(offset);
}
