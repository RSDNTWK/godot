#include "tests/test_macros.h"
TEST_FORCE_LINK(test_encrypted_safety)

#include "core/io/dir_access.h"
#include "core/io/file_access_encrypted.h"
#include "core/io/file_access_memory.h"
#include "core/io/marshalls.h"
#include "core/os/os.h"
#include "tests/core/io/encrypted_test_helpers.h"
#include "tests/test_utils.h"
#include <cstring>

namespace TestEncryptedSafety {
struct ErrorsOff {
	bool previous = CoreGlobals::print_error_enabled;
	ErrorsOff() { CoreGlobals::print_error_enabled = false; }
	~ErrorsOff() { CoreGlobals::print_error_enabled = previous; }
};
Vector<uint8_t> key() {
	Vector<uint8_t> result;
	REQUIRE(result.resize(32) == OK);
	for (int i = 0; i < 32; i++) { result.write[i] = i; }
	return result;
}
static Vector<uint8_t> payload(int p_size) {
	Vector<uint8_t> result;
	REQUIRE(result.resize(p_size) == OK);
	for (int i = 0; i < p_size; i++) { result.write[i] = i % 251; }
	return result;
}
Vector<uint8_t> stream(int p_size, bool p_magic, bool p_big_endian, int p_prefix) {
	const Vector<uint8_t> plain = payload(p_size);
	const int padded = (p_size + 15) / 16 * 16;
	const int header = p_magic ? 44 : 40;
	Vector<uint8_t> data;
	REQUIRE(data.resize(p_prefix + header + padded + 3) == OK);
	memset(data.ptrw(), 0xa7, data.size());
	uint8_t *out = data.ptrw() + p_prefix;
	if (p_magic) {
		uint32_t magic = ENCRYPTED_HEADER_MAGIC;
		if (p_big_endian) { magic = BSWAP32(magic); }
		memcpy(out, &magic, 4);
		out += 4;
	}
	REQUIRE(CryptoCore::md5(plain.ptr(), plain.size(), out) == OK);
	uint64_t size = p_size;
	if (p_big_endian) { size = BSWAP64(size); }
	memcpy(out + 16, &size, 8);
	Vector<uint8_t> iv;
	REQUIRE(iv.resize(16) == OK);
	for (int i = 0; i < 16; i++) { iv.write[i] = i + 16; }
	memcpy(out + 24, iv.ptr(), 16);
	if (padded) {
		memset(out + 40, 0, padded);
		if (p_size) { memcpy(out + 40, plain.ptr(), p_size); }
		const Vector<uint8_t> k = key();
		CryptoCore::AESContext context;
		REQUIRE(context.set_encode_key(k.ptr(), 256) == OK);
		REQUIRE(context.encrypt_cfb(padded, iv.ptrw(), out + 40, out + 40) == OK);
	}
	return data;
}
static Ref<FileAccessMemory> memory(const Vector<uint8_t> &p_data, uint64_t p_length, bool p_big_endian = false, int p_start = 0) {
	Ref<FileAccessMemory> file;
	file.instantiate();
	REQUIRE(file->open_custom(p_data.ptr(), p_length) == OK);
	file->set_big_endian(p_big_endian);
	file->seek(p_start);
	return file;
}
TEST_CASE("[N008][Encrypted] Independently encoded AES256 CFB bytes retain their format") {
	// Generated independently with OpenSSL enc -aes-256-cfb -nopad.
	const Vector<uint8_t> fixture = String("1bdd36b0a024c90db3835126072936921100000000000000101112131415161718191a1b1c1d1e1fe9c2ed89b63155e1f87d96dd3aeaa6815983a8fbeb938800b9b68fa664c50ed1").hex_decode();
	const Vector<uint8_t> k = key();
	FileAccessEncrypted reader;
	REQUIRE(reader.open_and_parse(memory(fixture, fixture.size()), k, FileAccessEncrypted::MODE_READ, false) == OK);
	CHECK(reader.FileAccess::get_buffer(17) == payload(17));
	CHECK(stream(17, false).slice(0, fixture.size()) == fixture);
	const String path = TestUtils::get_temp_path("encrypted_" + uitos(OS::get_singleton()->get_ticks_usec()));
	Ref<FileAccess> output = FileAccess::open(path, FileAccess::WRITE);
	REQUIRE(output.is_valid());
	FileAccessEncrypted writer;
	const Vector<uint8_t> original = stream(17, false);
	const Vector<uint8_t> iv = original.slice(24, 40);
	REQUIRE(writer.open_and_parse(output, k, FileAccessEncrypted::MODE_WRITE_AES256, false, iv) == OK);
	const Vector<uint8_t> plain = payload(17);
	REQUIRE(writer.store_buffer(plain.ptr(), plain.size()));
	writer.close();
	output->close();
	CHECK(FileAccess::get_file_as_bytes(path) == fixture);
	CHECK(DirAccess::remove_absolute(path) == OK);
}
TEST_CASE("[N008][Encrypted] Valid sizes, byte order, embedded offsets and EOF preserve exact plaintext") {
	const Vector<uint8_t> k = key();
	for (int size : { 0, 1, 15, 16, 17, 31, 32, 33, 1031 }) {
		for (bool magic : { false, true }) {
			for (bool big : { false, true }) {
				Vector<uint8_t> data = stream(size, magic, big, 5);
				Ref<FileAccessMemory> source = memory(data, data.size(), big, 5);
				FileAccessEncrypted reader;
				REQUIRE(reader.open_and_parse(source, k, FileAccessEncrypted::MODE_READ, magic) == OK);
				CHECK(source->get_position() == uint64_t(data.size() - 3));
				CHECK(reader.get_length() == uint64_t(size));
				uint8_t out[1040];
				memset(out, 0xa7, sizeof(out));
				CHECK(reader.get_buffer(out, size) == uint64_t(size));
				if (size) {
					const Vector<uint8_t> expected = payload(size);
					CHECK(memcmp(out, expected.ptr(), size) == 0);
				}
				CHECK(out[size] == 0xa7);
				CHECK(reader.get_error() == OK);
				CHECK(reader.get_buffer(out, 1) == 0);
				CHECK(reader.eof_reached());
				reader.seek(0);
				CHECK_FALSE(reader.eof_reached());
				CHECK(reader.FileAccess::get_buffer(size) == payload(size));
				reader.close();
				source->seek(5);
				CHECK(reader.open_and_parse(source, k, FileAccessEncrypted::MODE_READ, magic) == OK);
			}
		}
	}
}
TEST_CASE("[N008][Encrypted] Incomplete headers and padded payloads fail without retained contents") {
	ErrorsOff quiet;
	const Vector<uint8_t> k = key();
	for (bool magic : { false, true }) {
		const Vector<uint8_t> data = stream(17, magic);
		for (int size = 0; size < data.size() - 3; size++) {
			FileAccessEncrypted reader;
			CHECK(reader.open_and_parse(memory(data, size), k, FileAccessEncrypted::MODE_READ, magic) != OK);
			CHECK_FALSE(reader.is_open());
			CHECK(reader.get_length() == 0);
			CHECK(reader.get_iv().is_empty());
		}
		Vector<uint8_t> altered = data;
		altered.write[magic ? 4 : 0] ^= 1;
		FileAccessEncrypted reader;
		CHECK(reader.open_and_parse(memory(altered, altered.size()), k, FileAccessEncrypted::MODE_READ, magic) == ERR_FILE_CORRUPT);
		CHECK(reader.get_length() == 0);
		CHECK_FALSE(reader.is_open());
		CHECK(reader.open_and_parse(memory(data, data.size()), k, FileAccessEncrypted::MODE_READ, magic) == OK);
	}
}
class FaultReader : public FileAccessMemory {
public:
	uint64_t reported_length = 0;
	int fail_at = -1;
	mutable int calls = 0;
	mutable Error error = OK;
	uint64_t get_length() const override { return reported_length ? reported_length : FileAccessMemory::get_length(); }
	uint64_t get_buffer(uint8_t *p_dst, uint64_t p_length) const override {
		if (calls++ == fail_at) {
			error = ERR_FILE_CANT_READ;
			return FileAccessMemory::get_buffer(p_dst, p_length - 1);
		}
		return FileAccessMemory::get_buffer(p_dst, p_length);
	}
	Error get_error() const override { return error == OK ? FileAccessMemory::get_error() : error; }
};
TEST_CASE("[N008][Encrypted] Unsupported declared lengths are rejected before payload allocation or reads") {
	ErrorsOff quiet;
	const Vector<uint8_t> k = key();
	for (uint64_t size : { uint64_t(INT64_MAX), uint64_t(INT64_MAX) + 1, UINT64_MAX - 15, UINT64_MAX - 1, UINT64_MAX }) {
		Vector<uint8_t> data = stream(17, false);
		encode_uint64(size, data.ptrw() + 16);
		Ref<FaultReader> source;
		source.instantiate();
		REQUIRE(source->open_custom(data.ptr(), data.size()) == OK);
		source->reported_length = UINT64_MAX;
		FileAccessEncrypted reader;
		CHECK(reader.open_and_parse(source, k, FileAccessEncrypted::MODE_READ, false) == ERR_FILE_CORRUPT);
		CHECK(source->calls == 3);
		CHECK_FALSE(reader.is_open());
		CHECK(reader.get_length() == 0);
	}
}
TEST_CASE("[N008][Encrypted] Short backing reads retain their error and permit a clean retry") {
	ErrorsOff quiet;
	const Vector<uint8_t> k = key();
	const Vector<uint8_t> data = stream(17);
	for (int index = 0; index < 5; index++) {
		Ref<FaultReader> source;
		source.instantiate();
		REQUIRE(source->open_custom(data.ptr(), data.size()) == OK);
		source->fail_at = index;
		FileAccessEncrypted reader;
		CHECK(reader.open_and_parse(source, k, FileAccessEncrypted::MODE_READ) == ERR_FILE_CANT_READ);
		CHECK_FALSE(reader.is_open());
		CHECK(reader.get_length() == 0);
		CHECK(reader.get_iv().is_empty());
		source->seek(0);
		source->fail_at = -1;
		source->error = OK;
		CHECK(reader.open_and_parse(source, k, FileAccessEncrypted::MODE_READ) == OK);
		CHECK(reader.FileAccess::get_buffer(17) == payload(17));
	}
}
TEST_CASE("[N008][Encrypted] Invalid initialization cannot expose content retained from an earlier open") {
	ErrorsOff quiet;
	const Vector<uint8_t> k = key();
	const Vector<uint8_t> data = stream(17);
	for (int kind = 0; kind < 4; kind++) {
		FileAccessEncrypted reader;
		REQUIRE(reader.open_and_parse(memory(data, data.size()), k, FileAccessEncrypted::MODE_READ) == OK);
		reader.close();
		Ref<FileAccess> source;
		if (kind != 0) { source = memory(data, data.size()); }
		const Vector<uint8_t> supplied_key = kind == 1 ? Vector<uint8_t>() : k;
		const auto mode = kind == 2 ? FileAccessEncrypted::MODE_MAX : FileAccessEncrypted::MODE_READ;
		const Error error = reader.open_and_parse(source, supplied_key, mode, kind != 3);
		CHECK(error != OK);
		CHECK_FALSE(reader.is_open());
		CHECK(reader.get_length() == 0);
		CHECK(reader.get_iv().is_empty());
	}
	Ref<FileAccessMemory> source = memory(data, data.size());
	const Vector<uint8_t> bad_iv = key();
	FileAccessEncrypted writer;
	CHECK(writer.open_and_parse(source, k, FileAccessEncrypted::MODE_WRITE_AES256, true, bad_iv) == ERR_INVALID_PARAMETER);
	CHECK_FALSE(writer.is_open());
	CHECK(writer.get_length() == 0);
	CHECK(writer.get_iv().is_empty());
	CHECK(source->get_position() == 0);
}
TEST_CASE("[N008][Encrypted] Empty and ordinary password files preserve public writer and reader behavior") {
	const String path = TestUtils::get_temp_path("encrypted_password_" + uitos(OS::get_singleton()->get_ticks_usec()));
	for (int size : { 0, 17, 1031 }) {
		const Vector<uint8_t> plain = payload(size);
		Ref<FileAccess> writer = FileAccess::open_encrypted_pass(path, FileAccess::WRITE, "ordinary-test-password");
		REQUIRE(FileAccess::get_open_error() == OK);
		REQUIRE(writer.is_valid());
		REQUIRE(writer->store_buffer(plain.ptr(), plain.size()));
		writer->close();
		Ref<FileAccess> reader = FileAccess::open_encrypted_pass(path, FileAccess::READ, "ordinary-test-password");
		REQUIRE(FileAccess::get_open_error() == OK);
		REQUIRE(reader.is_valid());
		CHECK(reader->get_length() == uint64_t(size));
		CHECK(reader->get_buffer(size) == plain);
		reader->close();
	}
	CHECK(DirAccess::remove_absolute(path) == OK);
}
}
