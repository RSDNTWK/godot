/**************************************************************************/
/*  test_image_loader_staging.cpp                                         */
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
TEST_FORCE_LINK(test_zip_precedence)
#ifdef MINIZIP_ENABLED
#include "core/io/dir_access.h"
#include "core/io/file_access_memory.h"
#include "core/io/file_access_zip.h"
#include "core/io/zip_io.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"
#include "tests/test_utils.h"

namespace TestZIPPrecedence {
struct Fixture {
	static inline uint64_t sequence = 0;
	String token = "n038_" + uitos(OS::get_singleton()->get_ticks_usec()) + "_" + uitos(sequence++);
	String name = token + "/shared.txt";
	String path_a = TestUtils::get_temp_path(token + "_a.zip");
	String path_b = TestUtils::get_temp_path(token + "_b.zip");
	static void write(const String &p_path, const Vector<String> &p_names, const Vector<String> &p_values) {
		REQUIRE(p_names.size() == p_values.size());
		Ref<FileAccess> backing;
		zlib_filefunc_def io = zipio_create_io(&backing);
		zipFile zip = zipOpen2(p_path.utf8().get_data(), APPEND_STATUS_CREATE, nullptr, &io);
		REQUIRE(zip != nullptr);
		for (int i = 0; i < p_names.size(); i++) {
			const Vector<uint8_t> bytes = p_values[i].to_utf8_buffer();
			REQUIRE(zipOpenNewFileInZip(zip, p_names[i].utf8().get_data(), nullptr, nullptr, 0, nullptr, 0, nullptr, Z_DEFLATED, Z_DEFAULT_COMPRESSION) == ZIP_OK);
			REQUIRE(zipWriteInFileInZip(zip, bytes.ptr(), bytes.size()) == ZIP_OK);
			REQUIRE(zipCloseFileInZip(zip) == ZIP_OK);
		}
		REQUIRE(zipClose(zip, nullptr) == ZIP_OK);
	}
	void prepare() const {
		write(path_a, { name }, { "First" });
		write(path_b, { name }, { "Second" });
	}
	bool mount(bool p_second, bool p_replace) const {
		return ZipArchive::get_singleton()->try_open_pack(p_second ? path_b : path_a, p_replace, 0);
	}
	String read() const {
		Ref<FileAccess> file = FileAccess::open("res://" + name, FileAccess::READ);
		REQUIRE(file.is_valid());
		return file->get_as_utf8_string();
	}
	~Fixture() {
		PackedData::get_singleton()->remove_path("res://" + name);
		for (const String &path : { path_a, path_b }) {
			if (FileAccess::exists(path)) {
				DirAccess::remove_absolute(path);
			}
		}
	}
};
TEST_CASE("[N038][ZIPPrecedence] Replacement disabled and repeated mounts retain the selected archive") {
	Fixture fixture;
	fixture.prepare();
	REQUIRE(fixture.mount(false, true));
	Ref<FileAccess> first = FileAccess::open("res://" + fixture.name, FileAccess::READ);
	REQUIRE(first.is_valid());
	REQUIRE(fixture.mount(true, false));
	CHECK(fixture.read() == "First");
	REQUIRE(fixture.mount(true, true));
	CHECK(fixture.read() == "Second");
	first->seek(0);
	CHECK(first->get_as_utf8_string() == "First");
	REQUIRE(fixture.mount(false, false));
	CHECK(fixture.read() == "Second");
	REQUIRE(fixture.mount(false, true));
	CHECK(fixture.read() == "First");
	first.unref();
}
TEST_CASE("[N038][ZIPPrecedence] Canonical aliases within an archive obey replacement policy") {
	Fixture fixture;
	Fixture::write(fixture.path_a, { fixture.token + "/child/../shared.txt", fixture.name }, { "AliasFirst", "AliasSecond" });
	REQUIRE(fixture.mount(false, false));
	CHECK(fixture.read() == "AliasFirst");
	REQUIRE(fixture.mount(false, true));
	CHECK(fixture.read() == "AliasSecond");
}
class MemorySource : public PackSource {
public:
	Vector<uint8_t> bytes = String("Memory").to_utf8_buffer();
	bool try_open_pack(const String &, bool, uint64_t, const Vector<uint8_t> &) override { return false; }
	Ref<FileAccess> get_file(const String &, PackedData::PackedFile *, const Vector<uint8_t> &, const Vector<PackedData::PackedFile> *) override {
		Ref<FileAccessMemory> file;
		file.instantiate();
		if (file->open_custom(bytes.ptr(), bytes.size()) != OK) {
			return Ref<FileAccess>();
		}
		return file;
	}
};
TEST_CASE("[N038][ZIPPrecedence] ZIP mounts respect replacement across another pack provider") {
	Fixture fixture;
	fixture.prepare();
	REQUIRE(fixture.mount(false, true));
	MemorySource memory;
	uint8_t md5[16] = {};
	PackedData::get_singleton()->add_path("memory", "res://" + fixture.name, 1, memory.bytes.size(), memory.bytes.size(), md5, &memory, true, false);
	CHECK(fixture.read() == "Memory");
	REQUIRE(fixture.mount(true, false));
	CHECK(fixture.read() == "Memory");
	REQUIRE(fixture.mount(true, true));
	CHECK(fixture.read() == "Second");
	PackedData::get_singleton()->remove_path("res://" + fixture.name);
}
#ifdef THREADS_ENABLED
struct ConcurrentRead {
	String path;
	SafeFlag ready;
	SafeFlag stop;
	SafeFlag failed;
	static void run(void *p_data) {
		ConcurrentRead &state = *static_cast<ConcurrentRead *>(p_data);
		for (int i = 0; i < 2000 && !state.stop.is_set(); i++) {
			Ref<FileAccess> file = FileAccess::open(state.path, FileAccess::READ);
			if (file.is_null()) {
				state.failed.set();
			} else {
				const String value = file->get_as_utf8_string();
				if (value != "First" && value != "Second") {
					state.failed.set();
				}
			}
			state.ready.set();
		}
	}
};
TEST_CASE("[N038][ZIPPrecedence] Readers retain entry identity during concurrent mount table growth") {
	Fixture fixture;
	fixture.prepare();
	REQUIRE(fixture.mount(false, true));
	ConcurrentRead state;
	state.path = "res://" + fixture.name;
	Thread reader;
	reader.start(ConcurrentRead::run, &state);
	REQUIRE(reader.is_started());
	while (!state.ready.is_set()) {
		OS::get_singleton()->delay_usec(1000);
	}
	for (int i = 0; i < 20; i++) {
		CHECK(fixture.mount((i & 1) != 0, true));
	}
	state.stop.set();
	reader.wait_to_finish();
	CHECK_FALSE(state.failed.is_set());
}
#endif
} //namespace TestZIPPrecedence
#endif
