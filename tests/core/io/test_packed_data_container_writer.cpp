/**************************************************************************/
/*  test_packed_data_container_writer.cpp                                 */
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
TEST_FORCE_LINK(test_packed_data_container_writer)

#include "core/io/marshalls.h"
#include "core/io/packed_data_container.h"
#include "core/io/packed_data_container_internal.h"
#include "tests/core/io/allocation_test_helpers.h"

namespace TestPackedDataContainerWriter {
struct QuietErrors {
	bool previous = CoreGlobals::print_error_enabled;
	QuietErrors() { CoreGlobals::print_error_enabled = false; }
	~QuietErrors() { CoreGlobals::print_error_enabled = previous; }
};
static Array one(const Variant &p_value) {
	Array result;
	result.push_back(p_value);
	return result;
}
static Vector<uint8_t> bytes(const Ref<PackedDataContainer> &p_container) {
	return p_container->get("__data__");
}
TEST_CASE("[N031][PDCWriter] All previously omitted builtin values round trip") {
	const Variant values[] = {
		Vector2i(1, -2), Vector3i(1, -2, 3), Vector4(1, 2, 3, 4),
		Vector4i(1, -2, 3, -4), Rect2i(1, 2, 3, 4), Color(0.125, 0.25, 0.5, 1), Projection()
	};
	for (const Variant &value : values) {
		for (bool dictionary : { false, true }) {
			Ref<PackedDataContainer> packed = memnew(PackedDataContainer);
			Dictionary input;
			input["value"] = value;
			REQUIRE(packed->pack(dictionary ? Variant(input) : Variant(one(value))) == OK);
			bool valid = false;
			const Variant actual = packed->getvar(dictionary ? Variant("value") : Variant(0), &valid);
			CHECK(valid);
			CHECK(actual.get_type() == value.get_type());
			CHECK(actual == value);
		}
	}
}
TEST_CASE("[N031][PDCWriter] Legacy scalar bytes and repeated string offsets are unchanged") {
	Array input;
	input.push_back(42);
	input.push_back("shared");
	input.push_back("shared");
	input.push_back(Variant());
	Ref<PackedDataContainer> packed = memnew(PackedDataContainer);
	REQUIRE(packed->pack(input) == OK);
	const String golden = "feffffff0400000018000000200000002000000030000000020000002a0000000400000006000000736861726564000000000000";
	CHECK(String::hex_encode_buffer(bytes(packed).ptr(), bytes(packed).size()) == golden);
	bool valid = false;
	CHECK(packed->getvar(0, &valid) == Variant(42));
	CHECK(valid);
	CHECK(packed->getvar(1, &valid) == Variant("shared"));
	CHECK(valid);
	CHECK(packed->getvar(2, &valid) == Variant("shared"));
	CHECK(valid);
	CHECK(packed->getvar(3, &valid).get_type() == Variant::NIL);
	CHECK(valid);
}
TEST_CASE("[N031][PDCWriter] Unsupported values reject atomically and object handles retain NIL") {
	Ref<PackedDataContainer> packed = memnew(PackedDataContainer);
	REQUIRE(packed->pack(one(42)) == OK);
	const Vector<uint8_t> before = bytes(packed);
	for (const Variant &value : { Variant(Callable()), Variant(Signal()) }) {
		QuietErrors quiet;
		CHECK(packed->pack(one(value)) == ERR_INVALID_DATA);
		CHECK(bytes(packed) == before);
	}
	for (const Variant &value : { Variant(RID()), Variant((Object *)nullptr) }) {
		REQUIRE(packed->pack(one(value)) == OK);
		bool valid = false;
		CHECK(packed->getvar(0, &valid).get_type() == Variant::NIL);
		CHECK(valid);
	}
}
TEST_CASE("[N032][PDCWriter] Serialized extent arithmetic rejects before allocation or narrowing") {
	int64_t end = -1;
	CHECK(PackedDataContainerInternal::checked_extent(UINT32_MAX - 4, 4, end) == OK);
	CHECK(end == UINT32_MAX);
	for (const uint64_t extra : { uint64_t(1), UINT64_MAX }) {
		CHECK(PackedDataContainerInternal::checked_extent(UINT32_MAX, extra, end) == ERR_OUT_OF_MEMORY);
		CHECK(end == 0);
	}
	CHECK(PackedDataContainerInternal::checked_extent(UINT64_MAX, 0, end) == ERR_OUT_OF_MEMORY);
	CHECK(PackedDataContainerInternal::checked_extent(8, uint64_t(INT_MAX) * 12, end) == ERR_OUT_OF_MEMORY);
	CHECK(PackedDataContainerInternal::checked_extent(8, uint64_t(INT_MAX) * 4, end) == ERR_OUT_OF_MEMORY);
	for (uint64_t stride : { uint64_t(1), uint64_t(4), uint64_t(8), uint64_t(16) }) {
		CHECK(PackedDataContainerInternal::checked_scalar_array_extent((uint64_t(INT_MAX) - 11) / stride, stride) == OK);
		CHECK(PackedDataContainerInternal::checked_scalar_array_extent(uint64_t(INT_MAX), stride) == ERR_OUT_OF_MEMORY);
	}
}
TEST_CASE("[N033][PDCWriter] Direct and mixed cycles reject without replacing prior data") {
	QuietErrors quiet;
	Ref<PackedDataContainer> packed = memnew(PackedDataContainer);
	REQUIRE(packed->pack(one(42)) == OK);
	const Vector<uint8_t> before = bytes(packed);
	Array cycle;
	cycle.push_back(cycle);
	CHECK(packed->pack(cycle) == ERR_INVALID_DATA);
	CHECK(bytes(packed) == before);
	cycle.clear();
	Dictionary dictionary;
	dictionary["self"] = dictionary;
	CHECK(packed->pack(dictionary) == ERR_INVALID_DATA);
	CHECK(bytes(packed) == before);
	dictionary.clear();
	Array array;
	array.push_back(dictionary);
	dictionary["back"] = array;
	CHECK(packed->pack(array) == ERR_INVALID_DATA);
	CHECK(bytes(packed) == before);
	dictionary.clear();
	array.clear();
}
TEST_CASE("[N033][PDCWriter] Shared acyclic children remain legal and independent") {
	Array child = one(42);
	Array root;
	root.push_back(child);
	root.push_back(child);
	Ref<PackedDataContainer> packed = memnew(PackedDataContainer);
	REQUIRE(packed->pack(root) == OK);
	for (int i = 0; i < 2; i++) {
		bool valid = false;
		Ref<PackedDataContainerRef> nested = packed->getvar(i, &valid);
		REQUIRE(valid);
		REQUIRE(nested.is_valid());
		CHECK(nested->getvar(0, &valid) == Variant(42));
		CHECK(valid);
	}
}
TEST_CASE("[N033][PDCWriter] Existing Variant depth bound is enforced atomically") {
	Array root = one(42);
	for (int i = 1; i < Variant::MAX_RECURSION_DEPTH; i++) {
		root = one(root);
	}
	Ref<PackedDataContainer> packed = memnew(PackedDataContainer);
	REQUIRE(packed->pack(root) == OK);
	const Vector<uint8_t> before = bytes(packed);
	QuietErrors quiet;
	CHECK(packed->pack(one(root)) == ERR_OUT_OF_MEMORY);
	CHECK(bytes(packed) == before);
}
#ifdef B01_ALLOCATION_TESTS
TEST_CASE("[N032][PDCWriterAllocation] Initial table and later payload failures preserve old data") {
	for (bool payload : { false, true }) {
		Vector<uint8_t> content;
		REQUIRE(content.resize(1001) == OK);
		Array input = one(payload ? Variant(content) : Variant(42));
		Ref<PackedDataContainer> packed = memnew(PackedDataContainer);
		REQUIRE(packed->pack(one(7)) == OK);
		const Vector<uint8_t> before = bytes(packed);
		const size_t allocation = TestAllocation::allocation_size([&]() {
			Vector<uint8_t> probe;
			REQUIRE(probe.resize(payload ? 1024 : 12) == OK);
		});
		QuietErrors quiet;
		{
			TestAllocation::Failure failure(allocation);
			CHECK(packed->pack(input) == ERR_OUT_OF_MEMORY);
			CHECK(TestAllocation::failures() == 1);
		}
		CHECK(bytes(packed) == before);
		REQUIRE(packed->pack(input) == OK);
	}
}
#endif
} // namespace TestPackedDataContainerWriter
