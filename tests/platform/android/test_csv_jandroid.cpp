/**************************************************************************/
/*  test_csv_jandroid.cpp                                                 */
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

TEST_FORCE_LINK(test_csv_jandroid)

#if defined(C003_ANDROID_JNI_TESTS) && defined(LINUXBSD_ENABLED)
// Exercise the production backend on the host without replacing the platform JNI.
#define FileAccessFilesystemJAndroid FileAccessJAndroidCSVTest
#define get_jni_env get_csv_test_jni_env
#include "platform/android/file_access_filesystem_jandroid.cpp"
#undef get_jni_env
#undef FileAccessFilesystemJAndroid

#include "core/io/file_access_pack.h"
#include "core/os/os.h"
#include "editor/import/resource_importer_csv_translation.h"
#include "tests/core/io/csv_test_helpers.h"
#include "tests/test_utils.h"

#include <cstdarg>
#include <cstring>

static JNIEnv *csv_test_environment = nullptr;
JNIEnv *get_csv_test_jni_env() {
	return csv_test_environment;
}

namespace TestCSVAndroidJNI {
enum Method {
	M_OPEN = 1,
	M_SIZE,
	M_TELL,
	M_EOF,
	M_SET_EOF,
	M_SEEK,
	M_SEEK_END,
	M_READ,
	M_CLOSE,
	M_WRITE,
	M_FLUSH,
	M_EXISTS,
	M_MODIFIED,
	M_ACCESSED,
	M_RESIZE,
	M_FILE_SIZE
};
struct State {
	Vector<uint8_t> bytes;
	uint64_t position = 0;
	uint64_t fail_after = UINT64_MAX;
	void *buffer = nullptr;
	uint64_t capacity = 0;
	int read_calls = 0;
	int busy_reads = 0;
	int invalid_count = 0;
	bool eof = false;
	bool position_failure = false;
	bool size_failure = false;
	bool seek_failure = false;
	bool read_exception = false;
	bool buffer_failure = false;
	bool exception = false;
};
static State *current = nullptr;

static jobject JNICALL global_ref(JNIEnv *, jobject p_object) {
	return p_object;
}
static void JNICALL delete_ref(JNIEnv *, jobject) {}
static jclass JNICALL object_class(JNIEnv *, jobject) {
	return reinterpret_cast<jclass>(uintptr_t(1));
}
static jstring JNICALL new_string(JNIEnv *, const char *) {
	return reinterpret_cast<jstring>(uintptr_t(2));
}
static jmethodID JNICALL method_id(JNIEnv *, jclass, const char *p_name, const char *) {
	static const char *names[] = { "", "fileOpen", "fileGetSize", "fileGetPosition", "isFileEof", "setFileEof",
		"fileSeek", "fileSeekFromEnd", "fileRead", "fileClose", "fileWrite", "fileFlush", "fileExists",
		"fileLastModified", "fileLastAccessed", "fileResize", "fileSize" };
	for (uintptr_t i = 1; i < sizeof(names) / sizeof(names[0]); i++) {
		if (strcmp(names[i], p_name) == 0) {
			return reinterpret_cast<jmethodID>(i);
		}
	}
	return nullptr;
}
static jobject JNICALL direct_buffer(JNIEnv *, void *p_data, jlong p_capacity) {
	if (current->buffer_failure) {
		current->exception = true;
		return nullptr;
	}
	current->buffer = p_data;
	current->capacity = p_capacity;
	return reinterpret_cast<jobject>(uintptr_t(3));
}
static jboolean JNICALL exception_check(JNIEnv *) {
	return current->exception;
}
static void JNICALL exception_clear(JNIEnv *) {
	current->exception = false;
}
static jint JNICALL call_int(JNIEnv *, jobject, jmethodID p_method, va_list) {
	const uintptr_t method = reinterpret_cast<uintptr_t>(p_method);
	if (method == M_OPEN) {
		current->position = 0;
		current->eof = false;
		return 1;
	}
	if (method != M_READ) {
		return 0;
	}
	current->read_calls++;
	if (current->read_exception) {
		current->exception = true;
		return 0;
	}
	if (current->invalid_count < 0) {
		return -1;
	}
	if (current->invalid_count > 0) {
		return int(current->capacity + 1);
	}
	if (current->busy_reads > 0) {
		current->busy_reads--;
		return 0;
	}
	if (current->position >= current->fail_after) {
		// Java's IOException mapping is a negative count; EOF remains distinct.
		return -1;
	}
	if (current->position >= uint64_t(current->bytes.size())) {
		current->eof = true;
		return 0;
	}
	const uint64_t count = MIN(current->capacity, MIN(uint64_t(current->bytes.size()) - current->position, current->fail_after - current->position));
	memcpy(current->buffer, current->bytes.ptr() + current->position, count);
	current->position += count;
	return count;
}
static jlong JNICALL call_long(JNIEnv *, jobject, jmethodID p_method, va_list) {
	const uintptr_t method = reinterpret_cast<uintptr_t>(p_method);
	if (method == M_TELL) {
		return current->position_failure ? -1 : jlong(current->position);
	}
	if (method == M_SIZE || method == M_FILE_SIZE) {
		return current->size_failure ? -1 : current->bytes.size();
	}
	return 0;
}
static jboolean JNICALL call_boolean(JNIEnv *, jobject, jmethodID p_method, va_list) {
	return reinterpret_cast<uintptr_t>(p_method) == M_EOF ? current->eof : true;
}
static void JNICALL call_void(JNIEnv *, jobject, jmethodID p_method, va_list p_args) {
	const uintptr_t method = reinterpret_cast<uintptr_t>(p_method);
	if (method == M_SEEK || method == M_SEEK_END) {
		(void)va_arg(p_args, jint);
		const jlong offset = va_arg(p_args, jlong);
		if (!current->seek_failure) {
			current->position = method == M_SEEK ? uint64_t(offset) : uint64_t(jlong(current->bytes.size()) + offset);
			// FileChannelDataAccess.seek does not clear the Java EOF flag.
		}
	} else if (method == M_SET_EOF) {
		(void)va_arg(p_args, jint);
		current->eof = va_arg(p_args, int) != 0;
	}
}

struct Fixture {
	State state;
	JNINativeInterface_ functions = {};
	JNIEnv environment = {};
	TestCSVReadErrors::ErrorsOff quiet;
	explicit Fixture(const Vector<uint8_t> &p_data) {
		state.bytes = p_data;
		current = &state;
		functions.NewGlobalRef = global_ref;
		functions.DeleteGlobalRef = delete_ref;
		functions.DeleteLocalRef = delete_ref;
		functions.GetObjectClass = object_class;
		functions.NewStringUTF = new_string;
		functions.GetMethodID = method_id;
		functions.NewDirectByteBuffer = direct_buffer;
		functions.ExceptionCheck = exception_check;
		functions.ExceptionClear = exception_clear;
		functions.CallIntMethodV = call_int;
		functions.CallLongMethodV = call_long;
		functions.CallBooleanMethodV = call_boolean;
		functions.CallVoidMethodV = call_void;
		environment.functions = &functions;
		csv_test_environment = &environment;
		FileAccessJAndroidCSVTest::setup(reinterpret_cast<jobject>(uintptr_t(4)));
	}
	~Fixture() {
		FileAccessJAndroidCSVTest::terminate();
		csv_test_environment = nullptr;
		current = nullptr;
	}
};

TEST_CASE("[C003][AndroidJNI] Failed reads remain errors until seek or reopen") {
	const Vector<uint8_t> contents = String("\"first\nsecond\",tail\n").to_utf8_buffer();
	Fixture fixture(contents);
	FileAccessJAndroidCSVTest reader;
	REQUIRE(reader.open_internal("jni.csv", FileAccess::READ) == OK);
	fixture.state.fail_after = 10;
	CHECK(reader.get_csv_line().is_empty());
	CHECK(reader.get_error() == ERR_FILE_CANT_READ);
	CHECK_FALSE(reader.eof_reached());
	const int calls = fixture.state.read_calls;
	CHECK(reader.get_csv_line().is_empty());
	CHECK(fixture.state.read_calls == calls);
	fixture.state.fail_after = UINT64_MAX;
	reader.seek(0);
	CHECK(reader.get_error() == OK);
	Vector<String> row = reader.get_csv_line();
	REQUIRE(row.size() == 2);
	if (row.size() != 2) {
		return;
	}
	CHECK(row[0] == "first\nsecond");
	CHECK(row[1] == "tail");
	REQUIRE(reader.open_internal("jni.csv", FileAccess::READ) == OK);
	CHECK(reader.get_error() == OK);
	CHECK(reader.get_position() == 0);
}

TEST_CASE("[C003][AndroidJNI] Metadata and read-ahead seek failures reject the record") {
	for (int mode : { 0, 1, 2 }) {
		Fixture fixture(String("key,en\nHELLO,Hello\n").to_utf8_buffer());
		FileAccessJAndroidCSVTest reader;
		REQUIRE(reader.open_internal("jni.csv", FileAccess::READ) == OK);
		fixture.state.position_failure = mode == 0;
		fixture.state.size_failure = mode == 1;
		fixture.state.seek_failure = mode == 2;
		CHECK(reader.get_csv_line().is_empty());
		CHECK(reader.get_error() == ERR_FILE_CANT_READ);
		CHECK_FALSE(reader.eof_reached());
		if (mode != 2) {
			CHECK(fixture.state.read_calls == 0);
		}
	}
}

TEST_CASE("[C003][AndroidJNI] Invalid counts and JNI failures remain observable") {
	for (int mode : { 0, 1, 2, 3, 4 }) {
		Fixture fixture(String("a,b\n").to_utf8_buffer());
		FileAccessJAndroidCSVTest reader;
		REQUIRE(reader.open_internal("jni.csv", FileAccess::READ) == OK);
		fixture.state.invalid_count = mode == 0 ? -1 : mode == 1 ? 1
																 : 0;
		fixture.state.read_exception = mode == 2;
		fixture.state.buffer_failure = mode == 3;
		if (mode == 4) {
			csv_test_environment = nullptr;
		}
		CHECK(reader.get_csv_line().is_empty());
		CHECK(reader.get_error() == (mode == 4 ? ERR_UNCONFIGURED : ERR_FILE_CANT_READ));
		csv_test_environment = &fixture.environment;
		CHECK_FALSE(fixture.state.exception);
	}
}

TEST_CASE("[C003][AndroidJNI] Ordinary CRLF NUL EOF and retryable zero reads retain records") {
	const Vector<uint8_t> data = { 'a', 0, 'b', ',', 'c', '\r', '\n', 'd', ',', 'e' };
	Fixture fixture(data);
	FileAccessJAndroidCSVTest reader;
	REQUIRE(reader.open_internal("jni.csv", FileAccess::READ) == OK);
	fixture.state.busy_reads = 1;
	CHECK(reader.get_csv_line().is_empty());
	CHECK(reader.get_error() == ERR_BUSY);
	CHECK_FALSE(reader.eof_reached());
	Vector<String> row = reader.get_csv_line();
	REQUIRE(row.size() == 1);
	if (row.size() != 1) {
		return;
	}
	CHECK(row[0] == "a");
	row = reader.get_csv_line();
	REQUIRE(row.size() == 2);
	if (row.size() != 2) {
		return;
	}
	CHECK(row[0] == "b");
	CHECK(row[1] == "c");
	row = reader.get_csv_line();
	REQUIRE(row.size() == 2);
	if (row.size() != 2) {
		return;
	}
	CHECK(row[0] == "d");
	CHECK(row[1] == "e");
	CHECK(reader.eof_reached());
	CHECK(reader.get_error() == ERR_FILE_EOF);
	const int calls = fixture.state.read_calls;
	CHECK(reader.get_buffer(nullptr, 0) == 0);
	CHECK(fixture.state.read_calls == calls);
	CHECK(reader.get_error() == ERR_FILE_EOF);
	reader.seek_end(-3);
	CHECK(reader.get_error() == OK);
	CHECK_FALSE(reader.eof_reached());
	row = reader.get_csv_line();
	REQUIRE(row.size() == 2);
	if (row.size() != 2) {
		return;
	}
	CHECK(row[0] == "d");
	CHECK(row[1] == "e");
	CHECK(reader.eof_reached());
	reader.seek(0);
	CHECK(reader.get_error() == OK);
	CHECK_FALSE(reader.eof_reached());
	row = reader.get_csv_line();
	REQUIRE(row.size() == 1);
	if (row.size() != 1) {
		return;
	}
	CHECK(row[0] == "a");
}

struct Source : public PackSource {
	String path = TestUtils::get_temp_path("csv_jni_" + uitos(OS::get_singleton()->get_ticks_usec()) + ".csv");
	Ref<FileAccessJAndroidCSVTest> last_reader;
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
TEST_CASE("[C003][AndroidJNIImporter] Header and quoted or ordinary body failures propagate") {
	for (uint64_t cutoff : { uint64_t(3), uint64_t(12), uint64_t(25) }) {
		Fixture fixture(String("key,en\nHELLO,\"first\nsecond line\"\n").to_utf8_buffer());
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
} // namespace TestCSVAndroidJNI
#endif
