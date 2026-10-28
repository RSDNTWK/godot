#include "tests/test_macros.h"

TEST_FORCE_LINK(test_string_empty_append)

#include "core/string/ustring.h"

namespace TestStringEmptyAppend {

TEST_CASE("[StringEmptyAppend] Empty right operand preserves a nonempty string") {
	String value = "prefix";
	const int64_t previous_length = value.length();
	value += String();
	CHECK(value == "prefix");
	CHECK(value.length() == previous_length);
	CHECK((String("prefix") + String()) == "prefix");
}

TEST_CASE("[StringEmptyAppend] Empty and nonempty append controls preserve content") {
	String value;
	value += String();
	CHECK(value.is_empty());
	value += String("left");
	CHECK(value == "left");
	value += String("-right");
	CHECK(value == "left-right");
}

TEST_CASE("[StringEmptyAppend] Unicode and shared copy on write content is preserved") {
	const String original = String::utf8("Résumé 🌍");
	String value = original;
	String shared = value;
	value += String();
	CHECK(value == original);
	CHECK(shared == original);
	CHECK(value.length() == original.length());
	value += String::utf8(" ✓");
	CHECK(value == String::utf8("Résumé 🌍 ✓"));
	CHECK(shared == original);
}

} // namespace TestStringEmptyAppend
