#include "tests/test_macros.h"
TEST_FORCE_LINK(test_zip_safety)

#ifdef MINIZIP_ENABLED
#include "core/io/dir_access.h"
#include "core/io/file_access_memory.h"
#include "core/io/file_access_zip.h"
#include "core/io/marshalls.h"
#include "core/io/zip_io.h"
#include "core/os/os.h"
#include "tests/test_utils.h"
#include <cstring>

namespace TestZipSafety {

struct ErrorsOff {
	bool previous = CoreGlobals::print_error_enabled;
	ErrorsOff() { CoreGlobals::print_error_enabled = false; }
	~ErrorsOff() { CoreGlobals::print_error_enabled = previous; }
};

struct Fixture {
	String token = "zip_contract_" + uitos(OS::get_singleton()->get_ticks_usec()) + "/";
	String filename;
	Vector<String> names;
	bool mounted = false;
	explicit Fixture(const String &p_extension = "zip") {
		filename = TestUtils::get_temp_path(token.trim_suffix("/") + "." + p_extension);
	}
	static Vector<uint8_t> content(const String &p_name, int p_index) {
		if (p_name.ends_with("/empty")) {
			return Vector<uint8_t>();
		}
		if (!p_name.ends_with("/large")) {
			return ("payload-" + itos(p_index)).to_utf8_buffer();
		}
		Vector<uint8_t> bytes;
		REQUIRE(bytes.resize(40000) == OK);
		uint32_t state = 0x612fa392;
		for (int i = 0; i < bytes.size(); i++) {
			state ^= state << 13;
			state ^= state >> 17;
			state ^= state << 5;
			bytes.write[i] = state;
		}
		return bytes;
	}
	void write(const Vector<String> &p_names, int p_method = Z_DEFLATED) {
		names = p_names;
		Ref<FileAccess> backing;
		zlib_filefunc_def io = zipio_create_io(&backing);
		zipFile zip = zipOpen2(filename.utf8().get_data(), APPEND_STATUS_CREATE, nullptr, &io);
		REQUIRE(zip != nullptr);
		for (int i = 0; i < names.size(); i++) {
			const Vector<uint8_t> payload = content(names[i], i);
			REQUIRE(zipOpenNewFileInZip(zip, names[i].utf8().get_data(), nullptr, nullptr, 0, nullptr, 0, nullptr, p_method, Z_DEFAULT_COMPRESSION) == ZIP_OK);
			REQUIRE(zipWriteInFileInZip(zip, payload.ptr(), payload.size()) == ZIP_OK);
			REQUIRE(zipCloseFileInZip(zip) == ZIP_OK);
		}
		REQUIRE(zipClose(zip, nullptr) == ZIP_OK);
	}
	~Fixture() {
		if (mounted) {
			for (const String &name : names) {
				PackedData::get_singleton()->remove_path("res://" + name);
			}
		}
		DirAccess::remove_absolute(filename);
	}
};

TEST_CASE("[F06][ZIP] ZIP and PCZ mounts retain complete boundary and Unicode filenames") {
	for (const String &extension : { String("zip"), String("pcz") }) {
		Fixture fixture(extension);
		Vector<String> names;
		for (int size : { 255, 256, 257, 16384, 16385 }) {
			names.push_back(fixture.token + String("n").repeat(size - fixture.token.utf8().length()));
		}
		names.push_back(fixture.token + String("u").repeat(255 - fixture.token.utf8().length()) + String::utf8("\xc3\xa9") + ".txt");
		const String shared = fixture.token + String("s").repeat(270);
		names.push_back(shared + "one");
		names.push_back(shared + "two");
		names.push_back(fixture.token + "nested/ordinary.txt");
		fixture.write(names);
		REQUIRE(ZipArchive::get_singleton()->try_open_pack(fixture.filename, true, 0));
		fixture.mounted = true;
		for (int i = 0; i < names.size(); i++) {
			const String path = "res://" + names[i];
			CHECK(ZipArchive::get_singleton()->file_exists(path));
			Error error = FAILED;
			Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ, &error);
			REQUIRE(error == OK);
			REQUIRE(file.is_valid());
			CHECK(file->get_as_utf8_string() == "payload-" + itos(i));
		}
	}
}

TEST_CASE("[F06][ZIP] Metadata errors return before using filename size") {
	unz_file_info64 info = {};
	info.size_filename = 0xffffffffUL;
	String name = "unchanged";
	CHECK(godot_unzip_get_current_file_info(nullptr, info, name) == UNZ_PARAMERROR);
	CHECK(name == "unchanged");
}

class PartialReader : public FileAccessMemory {
public:
	const uint64_t *limit = nullptr;
	const bool *invalid_count = nullptr;
	const uint64_t *fail_after = nullptr;
	mutable bool failed = false;
	uint64_t get_buffer(uint8_t *p_dst, uint64_t p_length) const override {
		if (*invalid_count) {
			failed = true;
			return uint64_t(-1);
		}
		const uint64_t maximum = get_position() >= *fail_after ? MIN(*limit, uint64_t(2)) : *limit;
		const uint64_t read = FileAccessMemory::get_buffer(p_dst, MIN(p_length, maximum));
		failed = read < p_length && !FileAccessMemory::eof_reached();
		return read;
	}
	Error get_error() const override {
		return failed ? ERR_FILE_CANT_READ : FileAccessMemory::get_error();
	}
};

class MemorySource : public PackSource {
public:
	String path;
	Vector<uint8_t> bytes;
	uint64_t limit = UINT64_MAX;
	bool invalid_count = false;
	uint64_t fail_after = UINT64_MAX;
	explicit MemorySource(const Fixture &p_fixture) {
		path = p_fixture.filename + ".memory.zip";
		bytes = FileAccess::get_file_as_bytes(p_fixture.filename);
		const uint8_t md5[16] = {};
		PackedData::get_singleton()->add_path("", path, 1, bytes.size(), bytes.size(), md5, this, true, false);
	}
	~MemorySource() { PackedData::get_singleton()->remove_path(path); }
	bool try_open_pack(const String &, bool, uint64_t, const Vector<uint8_t> &) override { return false; }
	Ref<FileAccess> get_file(const String &, PackedData::PackedFile *, const Vector<uint8_t> &, const Vector<PackedData::PackedFile> *) override {
		Ref<PartialReader> reader;
		reader.instantiate();
		reader->limit = &limit;
		reader->invalid_count = &invalid_count;
		reader->fail_after = &fail_after;
		if (reader->open_custom(bytes.ptr(), bytes.size()) != OK) {
			return Ref<FileAccess>();
		}
		return reader;
	}
};

TEST_CASE("[F19][ZIP] Short and failed backing reads become persistent public errors") {
	for (int mode : { 0, 1, 2 }) {
		Fixture fixture;
		Vector<String> names;
		names.push_back(fixture.token + "entry.txt");
		fixture.write(names);
		MemorySource source(fixture);
		REQUIRE(ZipArchive::get_singleton()->try_open_pack(source.path, true, 0));
		fixture.mounted = true;
		const String logical = "res://" + names[0];
		Ref<FileAccess> file = FileAccess::open(logical, FileAccess::READ);
		REQUIRE(file.is_valid());
		source.limit = mode == 0 ? 3 : (mode == 1 ? 0 : UINT64_MAX);
		source.invalid_count = mode == 2;
		uint8_t output[32];
		memset(output, 0xa7, sizeof(output));
		CHECK(file->get_buffer(output, sizeof(output)) == 0);
		CHECK(file->get_error() == ERR_FILE_CANT_READ);
		CHECK_FALSE(file->eof_reached());
		for (uint8_t value : output) {
			CHECK(value == 0xa7);
		}
		CHECK(file->get_buffer(output, sizeof(output)) == 0);
		source.limit = UINT64_MAX;
		source.invalid_count = false;
		file->seek(0);
		CHECK(file->get_error() == OK);
		CHECK(file->get_as_utf8_string() == "payload-0");
		file->close();
		file = FileAccess::open(logical, FileAccess::READ);
		REQUIRE(file.is_valid());
		CHECK(file->get_as_utf8_string() == "payload-0");
	}
}

TEST_CASE("[F19][ZIP] Incomplete metadata cannot publish a mount") {
	Fixture fixture;
	Vector<String> names;
	names.push_back(fixture.token + "metadata.txt");
	fixture.write(names);
	MemorySource source(fixture);
	source.limit = 0;
	ErrorsOff quiet;
	CHECK_FALSE(ZipArchive::get_singleton()->try_open_pack(source.path, true, 0));
	CHECK_FALSE(PackedData::get_singleton()->has_path("res://" + names[0]));
}

TEST_CASE("[F19][ZIP] Ordinary reads, EOF and seek keep their behavior") {
	Fixture fixture;
	Vector<String> names;
	names.push_back(fixture.token + "control.txt");
	names.push_back(fixture.token + "empty");
	fixture.write(names);
	REQUIRE(ZipArchive::get_singleton()->try_open_pack(fixture.filename, true, 0));
	fixture.mounted = true;
	Ref<FileAccess> file = FileAccess::open("res://" + names[0], FileAccess::READ);
	REQUIRE(file.is_valid());
	uint8_t output[32] = {};
	CHECK(file->get_buffer(output, 9) == 9);
	CHECK(memcmp(output, "payload-0", 9) == 0);
	CHECK(file->get_error() == OK);
	CHECK(file->get_buffer(output, 1) == 0);
	CHECK(file->eof_reached());
	CHECK(file->get_error() == ERR_FILE_EOF);
	file->seek(2);
	CHECK_FALSE(file->eof_reached());
	CHECK(file->get_error() == OK);
	CHECK(file->get_buffer(output, 20) == 7);
	CHECK(memcmp(output, "yload-0", 7) == 0);
	Ref<FileAccess> empty = FileAccess::open("res://" + names[1], FileAccess::READ);
	REQUIRE(empty.is_valid());
	CHECK(empty->get_length() == 0);
	CHECK(empty->get_buffer(output, 1) == 0);
	CHECK(empty->eof_reached());
	CHECK(empty->get_error() == ERR_FILE_EOF);
}


TEST_CASE("[F19][ZIP] Read errors preserve progress from within a Minizip refill") {
	for (int method : { 0, Z_DEFLATED }) {
		Fixture fixture;
		Vector<String> names;
		names.push_back(fixture.token + "large");
		fixture.write(names, method);
		MemorySource source(fixture);
		REQUIRE(ZipArchive::get_singleton()->try_open_pack(source.path, true, 0));
		fixture.mounted = true;
		Ref<FileAccess> file = FileAccess::open("res://" + names[0], FileAccess::READ);
		REQUIRE(file.is_valid());
		source.fail_after = 30 + decode_uint16(source.bytes.ptr() + 26) + decode_uint16(source.bytes.ptr() + 28) + 16384;
		Vector<uint8_t> output;
		REQUIRE(output.resize(40016) == OK);
		memset(output.ptrw(), 0xa7, output.size());
		const uint64_t actual = file->get_buffer(output.ptrw(), 40000);
		REQUIRE(actual > 0);
		REQUIRE(actual < 40000);
		CHECK(file->get_position() == actual);
		CHECK(file->get_error() == ERR_FILE_CANT_READ);
		CHECK_FALSE(file->eof_reached());
		const Vector<uint8_t> expected = Fixture::content(names[0], 0);
		CHECK(memcmp(output.ptr(), expected.ptr(), actual) == 0);
		for (uint64_t i = actual; i < uint64_t(output.size()); i++) {
			CHECK(output[i] == 0xa7);
		}
		CHECK(file->get_buffer(output.ptrw(), 1) == 0);
	}
}

TEST_CASE("[F19][ZIP] Premature stream termination remains an I/O error") {
	for (int method : { 0, Z_DEFLATED }) {
		Fixture fixture;
		Vector<String> names;
		names.push_back(fixture.token + "entry.txt");
		fixture.write(names, method);
		MemorySource source(fixture);
		const uint32_t central = decode_uint32(source.bytes.ptr() + source.bytes.size() - 6);
		REQUIRE(decode_uint32(source.bytes.ptr() + central) == 0x02014b50);
		encode_uint32(10, source.bytes.ptrw() + 22);
		encode_uint32(10, source.bytes.ptrw() + central + 24);
		REQUIRE(ZipArchive::get_singleton()->try_open_pack(source.path, true, 0));
		fixture.mounted = true;
		Ref<FileAccess> file = FileAccess::open("res://" + names[0], FileAccess::READ);
		REQUIRE(file.is_valid());
		uint8_t output[32];
		memset(output, 0xa7, sizeof(output));
		CHECK(file->get_buffer(output, sizeof(output)) == 9);
		CHECK(file->get_position() == 9);
		CHECK(memcmp(output, "payload-0", 9) == 0);
		CHECK(output[9] == 0xa7);
		CHECK(file->get_error() == ERR_FILE_CANT_READ);
		CHECK_FALSE(file->eof_reached());
		CHECK(file->get_buffer(output, 1) == 0);
	}
}

} // namespace TestZipSafety
#endif
