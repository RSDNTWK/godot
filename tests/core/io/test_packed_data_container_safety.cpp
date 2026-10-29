/**************************************************************************/
/*  test_packed_data_container_safety.cpp                                                        */
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

TEST_FORCE_LINK(test_packed_data_container_safety)

#include "core/io/dir_access.h"
#include "core/io/marshalls.h"
#include "core/io/packed_data_container.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "tests/test_utils.h"

namespace TestPackedDataContainerSafety {
struct QuietErrors {
	bool previous = CoreGlobals::print_error_enabled;
	QuietErrors() { CoreGlobals::print_error_enabled = false; }
	~QuietErrors() { CoreGlobals::print_error_enabled = previous; }
};
static void word(Vector<uint8_t> &p_bytes, uint32_t p_value) {
	uint8_t encoded[4];
	encode_uint32(p_value, encoded);
	for (uint8_t byte : encoded) {
		p_bytes.push_back(byte);
	}
}
static Vector<uint8_t> header(uint32_t p_type, uint32_t p_count) {
	Vector<uint8_t> bytes;
	word(bytes, p_type);
	word(bytes, p_count);
	return bytes;
}
static void set_data(Ref<PackedDataContainer> p_container, const Vector<uint8_t> &p_bytes) {
	bool valid = false;
	p_container->set("__data__", p_bytes, &valid);
	REQUIRE(valid);
}
static Ref<PackedDataContainer> container(const Vector<uint8_t> &p_bytes) {
	Ref<PackedDataContainer> result = memnew(PackedDataContainer);
	set_data(result, p_bytes);
	return result;
}
static Vector<uint8_t> encoded(const Variant &p_value) {
	int length = 0;
	REQUIRE(encode_variant(p_value, nullptr, length, false) == OK);
	Vector<uint8_t> bytes;
	REQUIRE(bytes.resize(length) == OK);
	REQUIRE(encode_variant(p_value, bytes.ptrw(), length, false) == OK);
	return bytes;
}
static void invalid_root(Ref<PackedDataContainer> p_container) {
	CHECK(p_container->size() <= 0);
	bool valid = true;
	CHECK(p_container->getvar(0, &valid).get_type() == Variant::NIL);
	CHECK_FALSE(valid);
	Array iterator;
	iterator.push_back(0);
	CHECK_FALSE(bool(p_container->call("_iter_init", iterator)));
	CHECK_FALSE(bool(p_container->call("_iter_next", iterator)));
	CHECK(p_container->call("_iter_get", 0).get_type() == Variant::NIL);
}

TEST_CASE("[F04][PackedDataContainer] Incomplete headers and tables reject on each public read") {
	QuietErrors quiet;
	for (uint32_t type : { UINT32_MAX, UINT32_MAX - 1 }) {
		Vector<uint8_t> full = header(type, 1);
		word(full, 0);
		if (type == UINT32_MAX) {
			word(full, 0);
			word(full, 0);
		}
		for (int length = 0; length < full.size(); length++) {
			Vector<uint8_t> truncated = full;
			truncated.resize(length);
			invalid_root(container(truncated));
		}
		for (uint32_t count : { uint32_t(INT32_MAX), uint32_t(0x40000000), UINT32_MAX }) {
			Vector<uint8_t> impossible = full;
			encode_uint32(count, impossible.ptrw() + 4);
			invalid_root(container(impossible));
		}
		Ref<PackedDataContainer> empty = container(header(type, 0));
		CHECK(empty->size() == 0);
		Array iterator;
		iterator.push_back(0);
		CHECK_FALSE(bool(empty->call("_iter_init", iterator)));
	}
}

TEST_CASE("[F04][PackedDataContainer] Wrapped offsets and incomplete nested tables are invalid lookups") {
	QuietErrors quiet;
	for (uint32_t offset : { uint32_t(15), uint32_t(16), UINT32_MAX - 3, UINT32_MAX }) {
		Vector<uint8_t> bytes = header(UINT32_MAX - 1, 1);
		word(bytes, offset);
		word(bytes, 0); // Four-byte NIL leaf, available only at offset 12.
		Ref<PackedDataContainer> packed = container(bytes);
		CHECK(packed->size() == 1);
		bool valid = true;
		CHECK(packed->getvar(0, &valid).get_type() == Variant::NIL);
		CHECK_FALSE(valid);
		CHECK(packed->call("_iter_get", 0).get_type() == Variant::NIL);
	}
	for (uint32_t child_type : { UINT32_MAX, UINT32_MAX - 1 }) {
		for (bool count_present : { false, true }) {
			Vector<uint8_t> bytes = header(UINT32_MAX - 1, 1);
			word(bytes, 12);
			word(bytes, child_type);
			if (count_present) {
				word(bytes, 1); // Missing child's table.
			}
			Ref<PackedDataContainer> packed = container(bytes);
			bool valid = true;
			CHECK(packed->getvar(0, &valid).get_type() == Variant::NIL);
			CHECK_FALSE(valid);
		}
	}
}

TEST_CASE("[F04][PackedDataContainer] Valid NIL is distinct from truncated ordinary values") {
	QuietErrors quiet;
	Vector<uint8_t> nil = header(UINT32_MAX - 1, 1);
	word(nil, 12);
	word(nil, 0);
	Ref<PackedDataContainer> packed = container(nil);
	bool valid = false;
	CHECK(packed->getvar(0, &valid).get_type() == Variant::NIL);
	CHECK(valid);
	const Vector<uint8_t> scalar = encoded(int64_t(1) << 40);
	for (int length = 0; length < scalar.size(); length++) {
		Vector<uint8_t> partial = scalar;
		partial.resize(length);
		Vector<uint8_t> bytes = header(UINT32_MAX - 1, 1);
		word(bytes, 12);
		bytes.append_array(partial);
		set_data(packed, bytes);
		valid = true;
		CHECK(packed->getvar(0, &valid).get_type() == Variant::NIL);
		CHECK_FALSE(valid);
	}
	Array ordinary;
	ordinary.push_back("legacy");
	ordinary.push_back(42);
	Vector<uint8_t> bytes = header(UINT32_MAX - 1, 1);
	word(bytes, 12);
	bytes.append_array(encoded(ordinary));
	set_data(packed, bytes);
	valid = false;
	Variant result = packed->getvar(0, &valid);
	CHECK(valid);
	REQUIRE(result.get_type() == Variant::ARRAY);
	CHECK(Array(result) == ordinary);
}

TEST_CASE("[F04][PackedDataContainer] Dictionary key and value offset errors propagate") {
	QuietErrors quiet;
	Dictionary input;
	input["lookup"] = 42;
	Ref<PackedDataContainer> packed = memnew(PackedDataContainer);
	REQUIRE(packed->pack(input) == OK);
	const Vector<uint8_t> original = packed->get("__data__");
	REQUIRE(original.size() >= 20);
	for (int offset_position : { 12, 16 }) {
		Vector<uint8_t> bytes = original;
		encode_uint32(UINT32_MAX, bytes.ptrw() + offset_position);
		set_data(packed, bytes);
		bool valid = true;
		CHECK(packed->getvar("lookup", &valid).get_type() == Variant::NIL);
		CHECK_FALSE(valid);
		Array iterator;
		iterator.push_back(0);
		CHECK(bool(packed->call("_iter_init", iterator)));
		Variant key = packed->call("_iter_get", 0);
		if (offset_position == 16) {
			CHECK(key == Variant("lookup"));
		} else {
			CHECK(key.get_type() == Variant::NIL);
		}
	}
}

TEST_CASE("[F04][PackedDataContainer] Retained nested refs revalidate replacement owner data") {
	QuietErrors quiet;
	Array child;
	child.push_back("retained");
	Array input;
	input.push_back(child);
	Ref<PackedDataContainer> packed = memnew(PackedDataContainer);
	REQUIRE(packed->pack(input) == OK);
	bool valid = false;
	Ref<PackedDataContainerRef> nested = packed->getvar(0, &valid);
	REQUIRE(valid);
	REQUIRE(nested.is_valid());
	CHECK(nested->size() == 1);
	CHECK(nested->getvar(0, &valid) == Variant("retained"));
	CHECK(valid);
	Array replacement;
	replacement.push_back(42);
	REQUIRE(packed->pack(replacement) == OK);
	CHECK(nested->size() <= 0);
	valid = true;
	CHECK(nested->getvar(0, &valid).get_type() == Variant::NIL);
	CHECK_FALSE(valid);
	Array iterator;
	iterator.push_back(0);
	CHECK_FALSE(bool(nested->_iter_init(iterator)));
	CHECK(nested->_iter_get(0).get_type() == Variant::NIL);
	set_data(packed, Vector<uint8_t>());
	packed.unref();
	CHECK(nested->size() <= 0);
	valid = true;
	CHECK(nested->getvar(0, &valid).get_type() == Variant::NIL);
	CHECK_FALSE(valid);
}

TEST_CASE("[F04][PackedDataContainer] Nested containers and shared backward strings preserve behavior") {
	Array child;
	child.push_back("shared");
	child.push_back(Variant());
	Dictionary dictionary;
	dictionary["key"] = "shared";
	Array input;
	input.push_back("shared");
	input.push_back(child);
	input.push_back(dictionary);
	Ref<PackedDataContainer> packed = memnew(PackedDataContainer);
	REQUIRE(packed->pack(input) == OK);
	CHECK(packed->size() == 3);
	bool valid = false;
	CHECK(packed->getvar(0, &valid) == Variant("shared"));
	CHECK(valid);
	Ref<PackedDataContainerRef> nested = packed->getvar(1, &valid);
	REQUIRE(valid);
	REQUIRE(nested.is_valid());
	CHECK(nested->size() == 2);
	CHECK(nested->getvar(0, &valid) == Variant("shared"));
	CHECK(valid);
	CHECK(nested->getvar(1, &valid).get_type() == Variant::NIL);
	CHECK(valid);
	Ref<PackedDataContainerRef> nested_dictionary = packed->getvar(2, &valid);
	REQUIRE(valid);
	REQUIRE(nested_dictionary.is_valid());
	CHECK(nested_dictionary->getvar("key", &valid) == Variant("shared"));
	CHECK(valid);
	Array iterator;
	iterator.push_back(0);
	CHECK(bool(nested_dictionary->_iter_init(iterator)));
	CHECK(nested_dictionary->_iter_get(0) == Variant("key"));
	CHECK_FALSE(bool(nested_dictionary->_iter_next(iterator)));
}

TEST_CASE("[F04][PackedDataContainer] Existing serialization format round trips without changing bytes") {
	Dictionary input;
	input["first"] = 42;
	Array nested;
	nested.push_back("shared");
	nested.push_back("shared");
	input["second"] = nested;
	Ref<PackedDataContainer> packed = memnew(PackedDataContainer);
	REQUIRE(packed->pack(input) == OK);
	const Vector<uint8_t> original = packed->get("__data__");
	const String path = TestUtils::get_temp_path("f04_packed_container.res");
	REQUIRE(ResourceSaver::save(packed, path) == OK);
	Ref<PackedDataContainer> loaded = ResourceLoader::load(path, "PackedDataContainer", ResourceLoader::CACHE_MODE_IGNORE);
	REQUIRE(loaded.is_valid());
	CHECK(Vector<uint8_t>(loaded->get("__data__")) == original);
	bool valid = false;
	CHECK(loaded->getvar("first", &valid) == Variant(42));
	CHECK(valid);
	Ref<PackedDataContainerRef> result = loaded->getvar("second", &valid);
	REQUIRE(valid);
	REQUIRE(result.is_valid());
	CHECK(result->getvar(0, &valid) == Variant("shared"));
	CHECK(valid);
	CHECK(result->getvar(1, &valid) == Variant("shared"));
	CHECK(valid);
	CHECK(DirAccess::remove_absolute(path) == OK);
}
} // namespace TestPackedDataContainerSafety
