/**************************************************************************/
/*  test_csv_backend_errors.cpp                                           */
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

TEST_FORCE_LINK(test_csv_backend_errors)

#include "core/io/compression.h"
#include "core/io/dir_access.h"
#include "core/io/file_access_compressed.h"
#include "core/io/file_access_pack.h"
#include "core/io/marshalls.h"
#include "core/os/os.h"
#include "tests/core/io/csv_test_helpers.h"
#include "tests/test_utils.h"

#ifdef MINIZIP_ENABLED
#include "core/io/file_access_zip.h"
#include "core/io/zip_io.h"
#endif

#include <cstring>

namespace TestCSVReadErrors {
static Vector<uint8_t> csv_payload(int p_letters) {
	const CharString prefix = String("\"first\n").utf8();
	const CharString suffix = String("\",tail\n").utf8();
	Vector<uint8_t> data;
	if (data.resize(prefix.length() + p_letters + suffix.length()) != OK) {
		return Vector<uint8_t>();
	}
	memcpy(data.ptrw(), prefix.get_data(), prefix.length());
	uint32_t state = 0x612fa392;
	for (int i = 0; i < p_letters; i++) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		data.write[prefix.length() + i] = uint8_t('a' + state % 26);
	}
	memcpy(data.ptrw() + prefix.length() + p_letters, suffix.get_data(), suffix.length());
	return data;
}

#ifdef MINIZIP_ENABLED
struct ZipFixture {
	String token = "csv_zip_" + uitos(OS::get_singleton()->get_ticks_usec());
	String path = TestUtils::get_temp_path(token + ".zip");
	String entry = token + "/input.csv";
	bool mounted = false;
	Error write(const Vector<uint8_t> &p_data, int p_method) {
		Ref<FileAccess> file;
		zlib_filefunc_def io = zipio_create_io(&file);
		zipFile zip = zipOpen2(path.utf8().get_data(), APPEND_STATUS_CREATE, nullptr, &io);
		if (!zip) {
			return ERR_CANT_CREATE;
		}
		int error = zipOpenNewFileInZip(zip, entry.utf8().get_data(), nullptr, nullptr, 0, nullptr, 0, nullptr, p_method, Z_DEFAULT_COMPRESSION);
		if (error == ZIP_OK) {
			const int written = zipWriteInFileInZip(zip, p_data.ptr(), p_data.size());
			const int closed = zipCloseFileInZip(zip);
			error = written != ZIP_OK ? written : closed;
		}
		const int closed = zipClose(zip, nullptr);
		return error == ZIP_OK && closed == ZIP_OK ? OK : FAILED;
	}
	~ZipFixture() {
		if (mounted) {
			PackedData::get_singleton()->remove_path("res://" + entry);
		}
		DirAccess::remove_absolute(path);
	}
};

struct ZipSource : public PackSource {
	String path;
	Vector<uint8_t> bytes;
	Ref<Reader> last_reader;
	explicit ZipSource(const ZipFixture &p_fixture) {
		path = p_fixture.path + ".memory.zip";
		bytes = FileAccess::get_file_as_bytes(p_fixture.path);
		const uint8_t md5[16] = {};
		PackedData::get_singleton()->add_path("", path, 1, bytes.size(), bytes.size(), md5, this, true, false);
	}
	~ZipSource() { PackedData::get_singleton()->remove_path(path); }
	bool try_open_pack(const String &, bool, uint64_t, const Vector<uint8_t> &) override { return false; }
	Ref<FileAccess> get_file(const String &, PackedData::PackedFile *, const Vector<uint8_t> &, const Vector<PackedData::PackedFile> *) override {
		last_reader.instantiate();
		if (last_reader->open_bytes(bytes) != OK) {
			return Ref<FileAccess>();
		}
		return last_reader;
	}
};

TEST_CASE("[C003][CSVBackend] Stored and deflated ZIP read errors terminate multiline parsing") {
	ErrorsOff quiet;
	const Vector<uint8_t> payload = csv_payload(50000);
	REQUIRE(payload.size() > 50000);
	if (payload.size() <= 50000) {
		return;
	}
	for (int method : { 0, Z_DEFLATED }) {
		ZipFixture fixture;
		REQUIRE(fixture.write(payload, method) == OK);
		ZipSource source(fixture);
		REQUIRE(source.bytes.size() > 20000);
		if (source.bytes.size() <= 20000) {
			return;
		}
		REQUIRE(ZipArchive::get_singleton()->try_open_pack(source.path, true, 0));
		fixture.mounted = true;
		Ref<FileAccess> reader = FileAccess::open("res://" + fixture.entry, FileAccess::READ);
		REQUIRE(reader.is_valid());
		REQUIRE(source.last_reader.is_valid());
		if (reader.is_null() || source.last_reader.is_null()) {
			return;
		}
		// Let the first complete input refill supply the opening quoted line.
		source.last_reader->set_failure_position(20000);
		CHECK(reader->get_csv_line().is_empty());
		CHECK(reader->get_error() == ERR_FILE_CANT_READ);
		CHECK_FALSE(reader->eof_reached());
		reader.unref();
		Ref<FileAccess> normal = FileAccess::open("res://" + fixture.entry, FileAccess::READ);
		REQUIRE(normal.is_valid());
		if (normal.is_null()) {
			return;
		}
		const Vector<String> row = normal->get_csv_line();
		REQUIRE(row.size() == 2);
		if (row.size() != 2) {
			return;
		}
		CHECK(row[0].begins_with("first\n"));
		CHECK(row[0].length() == 50006);
		CHECK(row[1] == "tail");
	}
}
#endif

static Vector<uint8_t> compressed_csv(const Vector<uint8_t> &p_plain) {
	const uint32_t block_size = 32;
	const uint32_t count = p_plain.size() / block_size + 1;
	Vector<uint8_t> data;
	if (data.resize(16 + count * 4) != OK) {
		return Vector<uint8_t>();
	}
	memcpy(data.ptrw(), "GCMP", 4);
	encode_uint32(Compression::MODE_ZSTD, data.ptrw() + 4);
	encode_uint32(block_size, data.ptrw() + 8);
	encode_uint32(p_plain.size(), data.ptrw() + 12);
	for (uint32_t i = 0; i < count; i++) {
		const int length = MIN(uint64_t(block_size), uint64_t(p_plain.size()) - uint64_t(i) * block_size);
		const int64_t bound = Compression::get_max_compressed_buffer_size(length, Compression::MODE_ZSTD);
		Vector<uint8_t> encoded;
		if (bound < 0 || encoded.resize(bound) != OK) {
			return Vector<uint8_t>();
		}
		const int64_t size = Compression::compress(encoded.ptrw(), p_plain.ptr() + i * block_size, length, Compression::MODE_ZSTD);
		if (size < 0 || size > bound) {
			return Vector<uint8_t>();
		}
		encode_uint32(size, data.ptrw() + 16 + i * 4);
		const int64_t offset = data.size();
		if (data.resize(offset + size) != OK) {
			return Vector<uint8_t>();
		}
		if (size > 0) {
			memcpy(data.ptrw() + offset, encoded.ptr(), size);
		}
	}
	const int64_t offset = data.size();
	if (data.resize(offset + 4) != OK) {
		return Vector<uint8_t>();
	}
	memcpy(data.ptrw() + offset, "GCMP", 4);
	return data;
}

TEST_CASE("[C003][CSVBackend] A later compressed-block I/O error preserves the persistent error") {
	ErrorsOff quiet;
	const Vector<uint8_t> payload = csv_payload(100);
	const Vector<uint8_t> encoded = compressed_csv(payload);
	REQUIRE(encoded.size() > 32);
	if (encoded.size() <= 32) {
		return;
	}
	const uint32_t count = payload.size() / 32 + 1;
	const uint64_t second_block = 16 + count * 4 + decode_uint32(encoded.ptr() + 16);
	Ref<Reader> backing;
	backing.instantiate();
	REQUIRE(backing->open_bytes(encoded) == OK);
	backing->seek(4);
	FileAccessCompressed reader;
	REQUIRE(reader.open_after_magic(backing) == OK);
	backing->set_failure_position(second_block);
	CHECK(reader.get_csv_line().is_empty());
	CHECK(reader.get_error() == ERR_FILE_CANT_READ);
	CHECK_FALSE(reader.eof_reached());
	Ref<Reader> normal_backing;
	normal_backing.instantiate();
	REQUIRE(normal_backing->open_bytes(encoded) == OK);
	normal_backing->seek(4);
	FileAccessCompressed normal;
	REQUIRE(normal.open_after_magic(normal_backing) == OK);
	const Vector<String> row = normal.get_csv_line();
	REQUIRE(row.size() == 2);
	if (row.size() != 2) {
		return;
	}
	CHECK(row[0].begins_with("first\n"));
	CHECK(row[0].length() == 106);
	CHECK(row[1] == "tail");
}
} // namespace TestCSVReadErrors
