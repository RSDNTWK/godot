/**************************************************************************/
/*  test_csv_read_errors.cpp                                              */
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

TEST_FORCE_LINK(test_csv_read_errors)

#include "core/io/dir_access.h"
#include "core/io/file_access_memory.h"
#include "core/io/file_access_pack.h"
#include "core/io/resource_loader.h"
#include "core/os/os.h"
#include "core/string/translation.h"
#include "editor/import/resource_importer_csv_translation.h"
#include "tests/core/io/csv_test_helpers.h"
#include "tests/test_utils.h"

namespace TestCSVReadErrors {
struct Source : public PackSource {
	String path = TestUtils::get_temp_path("csv_contract_" + uitos(OS::get_singleton()->get_ticks_usec()) + ".csv");
	Vector<uint8_t> bytes;
	uint64_t failure_position;
	Ref<Reader> last_reader;
	Source(const String &p_contents, uint64_t p_failure_position = UINT64_MAX) :
			bytes(p_contents.to_utf8_buffer()), failure_position(p_failure_position) {
		const uint8_t md5[16] = {};
		PackedData::get_singleton()->add_path("", path, 1, bytes.size(), bytes.size(), md5, this, true, false);
	}
	~Source() { PackedData::get_singleton()->remove_path(path); }
	bool try_open_pack(const String &, bool, uint64_t, const Vector<uint8_t> &) override { return false; }
	Ref<FileAccess> get_file(const String &, PackedData::PackedFile *, const Vector<uint8_t> &, const Vector<PackedData::PackedFile> *) override {
		last_reader.instantiate();
		if (last_reader->open_bytes(bytes, failure_position) != OK) {
			return Ref<FileAccess>();
		}
		return last_reader;
	}
};

TEST_CASE("[C003][CSV] Persistent non-EOF errors terminate quoted and ordinary records") {
	ErrorsOff quiet;
	const Vector<uint8_t> contents = String("\"first\nsecond line\",tail\nordinary,row\n").to_utf8_buffer();
	for (uint64_t cutoff : { uint64_t(0), uint64_t(3), uint64_t(10), uint64_t(29) }) {
		Reader reader;
		REQUIRE(reader.open_bytes(contents, cutoff) == OK);
		if (cutoff > 25) {
			CHECK(reader.get_csv_line().size() == 2);
		}
		CHECK(reader.get_csv_line().is_empty());
		CHECK(reader.get_error() == ERR_FILE_CANT_READ);
		CHECK_FALSE(reader.eof_reached());
		CHECK(reader.failed_reads == 1);
		CHECK(reader.get_csv_line().is_empty());
		CHECK(reader.failed_reads == 2);
	}
}

TEST_CASE("[C003][CSV] UTF8 escaped quotes multiline delimiters CRLF and true EOF retain rows") {
	const Vector<uint8_t> contents = String::utf8("key;\"h\u00e9llo \"\"world\"\"\r\nsecond\"\r\n\r\nlast;tail").to_utf8_buffer();
	Reader reader;
	REQUIRE(reader.open_bytes(contents) == OK);
	Vector<String> row = reader.get_csv_line(";");
	REQUIRE(row.size() == 2);
	if (row.size() != 2) {
		return;
	}
	CHECK(row[0] == "key");
	CHECK(row[1] == String::utf8("h\u00e9llo \"world\"\nsecond"));
	row = reader.get_csv_line(";");
	REQUIRE(row.size() == 1);
	if (row.size() != 1) {
		return;
	}
	CHECK(row[0].is_empty());
	row = reader.get_csv_line(";");
	REQUIRE(row.size() == 2);
	if (row.size() != 2) {
		return;
	}
	CHECK(row[0] == "last");
	CHECK(row[1] == "tail");
	CHECK(reader.eof_reached());
	CHECK(reader.get_error() == ERR_FILE_EOF);
}

static HashMap<StringName, Variant> options() {
	HashMap<StringName, Variant> result;
	result["delimiter"] = 0;
	result["compress"] = 0;
	return result;
}

TEST_CASE("[C003][CSVImporter] Header and body errors propagate before indexing continuing or saving") {
	ErrorsOff quiet;
	const String contents = "key,en\nHELLO,\"first\nsecond part that continues\"\n";
	for (uint64_t cutoff : { uint64_t(3), uint64_t(12), uint64_t(25) }) {
		Source source(contents, cutoff);
		ResourceImporterCSVTranslation importer;
		List<String> generated;
		CHECK(importer.import(ResourceUID::INVALID_ID, source.path, source.path + ".unused", options(), nullptr, &generated, nullptr) == ERR_FILE_CANT_READ);
		CHECK(generated.is_empty());
		REQUIRE(source.last_reader.is_valid());
		if (source.last_reader.is_null()) {
			return;
		}
		CHECK(source.last_reader->get_error() == ERR_FILE_CANT_READ);
		CHECK(source.last_reader->failed_reads == 1);
		CHECK_FALSE(FileAccess::exists(source.path.get_basename() + ".en.translation"));
	}
}

struct Generated {
	List<String> paths;
	~Generated() {
		for (const String &path : paths) {
			const ResourceUID::ID uid = ResourceLoader::get_resource_uid(path);
			if (uid != ResourceUID::INVALID_ID && ResourceUID::get_singleton()->has_id(uid) &&
					ResourceUID::get_singleton()->get_id_path(uid) == path) {
				ResourceUID::get_singleton()->remove_id(uid);
			}
			DirAccess::remove_absolute(path);
		}
	}
};

TEST_CASE("[C003][CSVImporter] Ordinary unterminated final translation row remains importable") {
	Source source("key,en\nHELLO,Hello");
	ResourceImporterCSVTranslation importer;
	Generated generated;
	CHECK(importer.import(ResourceUID::get_singleton()->create_id(), source.path, source.path + ".unused", options(), nullptr, &generated.paths, nullptr) == OK);
	REQUIRE(generated.paths.size() == 1);
	if (generated.paths.size() != 1) {
		return;
	}
	Ref<Translation> translation = ResourceLoader::load(generated.paths.front()->get());
	REQUIRE(translation.is_valid());
	if (translation.is_null()) {
		return;
	}
	CHECK(translation->get_message("HELLO") == StringName("Hello"));
}
} // namespace TestCSVReadErrors
