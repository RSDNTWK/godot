/**************************************************************************/
/*  test_csv_android_asset.cpp                                            */
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

TEST_FORCE_LINK(test_csv_android_asset)

#if defined(C003_ANDROID_JNI_TESTS) && defined(LINUXBSD_ENABLED)
#define FileAccessAndroid FileAccessAndroidCSVTest
#define get_jni_env get_asset_csv_test_jni_env
#include "platform/android/file_access_android.cpp"
#undef get_jni_env
#undef FileAccessAndroid

#include "core/io/file_access_pack.h"
#include "core/os/os.h"
#include "editor/import/resource_importer_csv_translation.h"
#include "tests/core/io/csv_test_helpers.h"
#include "tests/test_utils.h"

#include <cstring>

namespace TestCSVAndroidAsset {
struct State {
	Vector<uint8_t> bytes;
	uint64_t position = 0;
	uint64_t fail_after = UINT64_MAX;
	int64_t reported_length = INT64_MIN;
	int reads = 0;
	int opens = 0;
	int closes = 0;
	int invalid_handle_queries = 0;
	String last_opened_path;
	bool open_failure = false;
	int errors = 0;
	int seeks = 0;
	bool seek_failure = false;
	bool invalid_count = false;
};
static State *current = nullptr;
static JNIEnv *environment = nullptr;
} //namespace TestCSVAndroidAsset
JNIEnv *get_asset_csv_test_jni_env() {
	return TestCSVAndroidAsset::environment;
}
extern "C" {
AAsset *AAssetManager_open(AAssetManager *, const char *p_path, int) {
	using namespace TestCSVAndroidAsset;
	current->opens++;
	current->last_opened_path = String::utf8(p_path);
	if (current->open_failure) {
		return nullptr;
	}
	current->position = 0;
	return reinterpret_cast<AAsset *>(current);
}
void AAsset_close(AAsset *) {
	TestCSVAndroidAsset::current->closes++;
}
int64_t AAsset_getLength64(AAsset *p_asset) {
	using namespace TestCSVAndroidAsset;
	if (!p_asset) {
		current->invalid_handle_queries++;
		return -1;
	}
	return current->reported_length == INT64_MIN ? current->bytes.size() : current->reported_length;
}
int64_t AAsset_seek64(AAsset *, int64_t p_offset, int p_whence) {
	using namespace TestCSVAndroidAsset;
	current->seeks++;
	if (current->seek_failure) {
		return -1;
	}
	const int64_t base = p_whence == SEEK_END ? (current->reported_length == INT64_MIN ? int64_t(current->bytes.size()) : current->reported_length) : 0;
	if (p_offset < -base || p_offset > INT64_MAX - base) {
		return -1;
	}
	current->position = uint64_t(base + p_offset);
	return current->position;
}
int AAsset_read(AAsset *, void *p_data, size_t p_bytes) {
	using namespace TestCSVAndroidAsset;
	current->reads++;
	if (current->invalid_count) {
		return int(p_bytes + 1);
	}
	if (current->position >= current->fail_after) {
		current->errors++;
		if (current->errors <= 32) {
			return -1;
		}
		// Finite test escape if a regression ignores the first persistent error.
		current->fail_after = UINT64_MAX;
	}
	if (current->position >= uint64_t(current->bytes.size())) {
		return 0;
	}
	const uint64_t count = MIN(uint64_t(p_bytes), MIN(uint64_t(current->bytes.size()) - current->position, current->fail_after - current->position));
	memcpy(p_data, current->bytes.ptr() + current->position, count);
	current->position += count;
	return int(count);
}
AAssetManager *AAssetManager_fromJava(JNIEnv *, jobject) {
	return reinterpret_cast<AAssetManager *>(uintptr_t(1));
}
}

namespace TestCSVAndroidAsset {
static jobject JNICALL global_ref(JNIEnv *, jobject p_object) {
	return p_object;
}
static void JNICALL delete_ref(JNIEnv *, jobject) {}
struct Fixture {
	State state;
	JNINativeInterface_ functions = {};
	JNIEnv jni = {};
	TestCSVReadErrors::ErrorsOff quiet;
	explicit Fixture(const String &p_contents) {
		state.bytes = p_contents.to_utf8_buffer();
		current = &state;
		functions.NewGlobalRef = global_ref;
		functions.DeleteGlobalRef = delete_ref;
		jni.functions = &functions;
		environment = &jni;
		FileAccessAndroidCSVTest::setup(reinterpret_cast<jobject>(uintptr_t(4)));
	}
	~Fixture() {
		FileAccessAndroidCSVTest::terminate();
		environment = nullptr;
		current = nullptr;
	}
};

TEST_CASE("[C003][AndroidAsset] Negative reads terminate records and remain errors until recovery") {
	for (const String &text : { String("\"first\nsecond line\",tail\n"), String("ordinary,row\n") }) {
		Fixture fixture(text);
		FileAccessAndroidCSVTest reader;
		REQUIRE(reader.open_internal("res://csv.csv", FileAccess::READ) == OK);
		fixture.state.fail_after = 10;
		CHECK(reader.get_csv_line().is_empty());
		CHECK(reader.get_error() == ERR_FILE_CANT_READ);
		CHECK_FALSE(reader.eof_reached());
		CHECK(fixture.state.errors == 1);
		const int calls = fixture.state.reads;
		CHECK(reader.get_csv_line().is_empty());
		CHECK(fixture.state.reads == calls);
		fixture.state.fail_after = UINT64_MAX;
		reader.seek(0);
		CHECK(reader.get_error() == OK);
		CHECK(reader.get_csv_line().size() == 2);
	}
}

TEST_CASE("[C003][AndroidAsset] Ordinary multiline CRLF EOF and seek or reopen retain records") {
	Fixture fixture("\"first\nsecond\",tail\r\nlast,end");
	FileAccessAndroidCSVTest reader;
	REQUIRE(reader.open_internal("res://csv.csv", FileAccess::READ) == OK);
	Vector<String> row = reader.get_csv_line();
	REQUIRE(row.size() == 2);
	if (row.size() != 2) {
		return;
	}
	CHECK(row[0] == "first\nsecond");
	CHECK(row[1] == "tail");
	row = reader.get_csv_line();
	REQUIRE(row.size() == 2);
	if (row.size() != 2) {
		return;
	}
	CHECK(row[0] == "last");
	CHECK(row[1] == "end");
	CHECK(reader.get_error() == ERR_FILE_EOF);
	const int reads = fixture.state.reads;
	CHECK(reader.get_buffer(nullptr, 0) == 0);
	CHECK(fixture.state.reads == reads);
	reader.seek_end(-8);
	CHECK(reader.get_error() == OK);
	CHECK_FALSE(reader.eof_reached());
	CHECK(reader.get_csv_line() == row);
	reader.seek(0);
	CHECK(reader.get_error() == OK);
	CHECK(reader.get_csv_line().size() == 2);
	REQUIRE(reader.open_internal("res://csv.csv", FileAccess::READ) == OK);
	CHECK(reader.get_position() == 0);
	CHECK(reader.get_error() == OK);
}

TEST_CASE("[C003][AndroidAsset] Seek metadata invalid counts and closed readers report failures") {
	Fixture fixture("a,b\n");
	FileAccessAndroidCSVTest reader;
	REQUIRE(reader.open_internal("res://csv.csv", FileAccess::READ) == OK);
	fixture.state.invalid_count = true;
	uint8_t byte = 0;
	CHECK(reader.get_buffer(&byte, 1) == 0);
	CHECK(reader.get_error() == ERR_FILE_CANT_READ);
	fixture.state.invalid_count = false;
	reader.seek(0);
	CHECK(reader.get_error() == OK);
	fixture.state.seek_failure = true;
	reader.seek(0);
	CHECK(reader.get_error() == ERR_FILE_CANT_READ);
	fixture.state.seek_failure = false;
	reader.seek(0);
	CHECK(reader.get_error() == OK);
	reader.close();
	CHECK(reader.get_buffer(&byte, 1) == 0);
	CHECK(reader.get_error() == ERR_UNCONFIGURED);
	fixture.state.reported_length = -1;
	CHECK(reader.open_internal("res://csv.csv", FileAccess::READ) == ERR_FILE_CANT_READ);
	CHECK_FALSE(reader.is_open());
	CHECK(reader.get_error() == ERR_FILE_CANT_READ);
}

TEST_CASE("[C003][AndroidAsset] Wide metadata and seek offsets are checked without narrowing") {
	Fixture fixture("a,b\n");
	fixture.state.reported_length = (int64_t(1) << 33) + 7;
	FileAccessAndroidCSVTest reader;
	REQUIRE(reader.open_internal("res://csv.csv", FileAccess::READ) == OK);
	CHECK(reader.get_length() == uint64_t(fixture.state.reported_length));
	reader.seek((uint64_t(1) << 32) + 3);
	CHECK(reader.get_position() == (uint64_t(1) << 32) + 3);
	CHECK(reader.get_error() == OK);
	reader.seek_end(-2);
	CHECK(reader.get_position() == uint64_t(fixture.state.reported_length - 2));
	CHECK(reader.get_error() == OK);
	const int calls = fixture.state.seeks;
	reader.seek_end(INT64_MAX);
	CHECK(reader.get_error() == ERR_FILE_CANT_READ);
	CHECK(fixture.state.seeks == calls);
	reader.seek(UINT64_MAX);
	CHECK(reader.get_error() == ERR_FILE_CANT_READ);
	CHECK(fixture.state.seeks == calls);
	reader.seek(0);
	CHECK(reader.get_error() == OK);
}

TEST_CASE("[N017][AndroidAssetMetadata] Named size queries open their own asset and release it") {
	Fixture fixture("a,b\n");
	FileAccessAndroidCSVTest metadata;
	CHECK_FALSE(metadata.is_open());
	CHECK(metadata._get_size("res://requested.csv") == 4);
	CHECK(fixture.state.last_opened_path == "requested.csv");
	CHECK(fixture.state.opens == 1);
	CHECK(fixture.state.closes == 1);
	CHECK(fixture.state.invalid_handle_queries == 0);
	CHECK_FALSE(metadata.is_open());
	fixture.state.reported_length = (int64_t(1) << 33) + 7;
	CHECK(metadata._get_size("res://large.csv") == fixture.state.reported_length);
	CHECK(fixture.state.opens == 2);
	CHECK(fixture.state.closes == 2);
	fixture.state.open_failure = true;
	CHECK(metadata._get_size("res://missing.csv") == -1);
	CHECK(fixture.state.last_opened_path == "missing.csv");
	CHECK(fixture.state.opens == 3);
	CHECK(fixture.state.closes == 2);
	CHECK(fixture.state.invalid_handle_queries == 0);
}

struct Source : public PackSource {
	String path = TestUtils::get_temp_path("csv_asset_" + uitos(OS::get_singleton()->get_ticks_usec()) + ".csv");
	Ref<FileAccessAndroidCSVTest> last_reader;
	Source() {
		const uint8_t md5[16] = {};
		PackedData::get_singleton()->add_path("", path, 1, current->bytes.size(), current->bytes.size(), md5, this, true, false);
	}
	~Source() { PackedData::get_singleton()->remove_path(path); }
	bool try_open_pack(const String &, bool, uint64_t, const Vector<uint8_t> &) override { return false; }
	Ref<FileAccess> get_file(const String &, PackedData::PackedFile *, const Vector<uint8_t> &, const Vector<PackedData::PackedFile> *) override {
		last_reader.instantiate();
		if (last_reader->open_internal(path, FileAccess::READ) != OK) {
			return Ref<FileAccess>();
		}
		return last_reader;
	}
};
TEST_CASE("[C003][AndroidAssetImporter] Header ordinary and quoted body read failures propagate") {
	for (const String &text : { String("key,en\nHELLO,Hello\n"), String("key,en\nHELLO,\"first\nsecond line\"\n") }) {
		for (uint64_t cutoff : { uint64_t(3), uint64_t(12), uint64_t(25) }) {
			if (cutoff >= uint64_t(text.utf8().length())) {
				continue;
			}
			Fixture fixture(text);
			fixture.state.fail_after = cutoff;
			Source source;
			ResourceImporterCSVTranslation importer;
			HashMap<StringName, Variant> options;
			options["delimiter"] = 0;
			options["compress"] = 0;
			List<String> generated;
			CHECK(importer.import(ResourceUID::INVALID_ID, source.path, source.path + ".unused", options, nullptr, &generated, nullptr) == ERR_FILE_CANT_READ);
			CHECK(generated.is_empty());
			REQUIRE(source.last_reader.is_valid());
			if (source.last_reader.is_null()) {
				return;
			}
			CHECK(source.last_reader->get_error() == ERR_FILE_CANT_READ);
			CHECK_FALSE(source.last_reader->eof_reached());
		}
	}
}
} // namespace TestCSVAndroidAsset
#endif
