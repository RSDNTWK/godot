/**************************************************************************/
/*  file_access_encrypted.cpp                                             */
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

#include "file_access_encrypted.h"

#include "core/variant/variant.h"

CryptoCore::RandomGenerator *FileAccessEncrypted::_fae_static_rng = nullptr;

void FileAccessEncrypted::deinitialize() {
	if (_fae_static_rng) {
		memdelete(_fae_static_rng);
		_fae_static_rng = nullptr;
	}
}

Error FileAccessEncrypted::open_and_parse(Ref<FileAccess> p_base, const Vector<uint8_t> &p_key, Mode p_mode, bool p_with_magic, const Vector<uint8_t> &p_iv) {
	ERR_FAIL_COND_V(file.is_valid(), ERR_ALREADY_IN_USE);

	pos = 0;
	eofed = false;
	use_magic = p_with_magic;
	length = 0;
	base = 0;
	data.clear();
	iv.clear();
	key.clear();
	auto fail = [&](Error p_error) {
		data.clear();
		iv.clear();
		key.clear();
		length = 0;
		base = 0;
		pos = 0;
		eofed = false;
		writing = false;
		return p_error;
	};
	if (p_base.is_null() || !p_base->is_open() || p_key.size() != 32 || p_mode < MODE_READ || p_mode >= MODE_MAX) {
		return fail(ERR_INVALID_PARAMETER);
	}
	key = p_key;
	if (p_mode == MODE_WRITE_AES256) {
		writing = true;
		if (!p_iv.is_empty() && p_iv.size() != 16) {
			return fail(ERR_INVALID_PARAMETER);
		}
		const Error allocation = iv.resize(16);
		if (allocation != OK) {
			return fail(allocation);
		}
		if (p_iv.is_empty()) {
			if (unlikely(!_fae_static_rng)) {
				_fae_static_rng = memnew(CryptoCore::RandomGenerator);
				const Error initialized = _fae_static_rng->init();
				if (initialized != OK) {
					memdelete(_fae_static_rng);
					_fae_static_rng = nullptr;
					return fail(initialized);
				}
			}
			const Error randomized = _fae_static_rng->get_random_bytes(iv.ptrw(), 16);
			if (randomized != OK) {
				return fail(randomized);
			}
		} else {
			memcpy(iv.ptrw(), p_iv.ptr(), 16);
		}
		file = p_base;
		return OK;
	}
	writing = false;
	const uint64_t start = p_base->get_position();
	const uint64_t source_length = p_base->get_length();
	const uint64_t header_size = use_magic ? 44 : 40;
	if (start > source_length || header_size > source_length - start) {
		return fail(ERR_FILE_CORRUPT);
	}
	auto read_exact = [&](uint8_t *p_target, uint64_t p_count) {
		const uint64_t read = p_base->get_buffer(p_target, p_count);
		const Error error = p_base->get_error();
		if (read != p_count) {
			return error != OK && error != ERR_FILE_EOF ? error : ERR_FILE_CORRUPT;
		}
		return error == ERR_FILE_EOF ? OK : error;
	};
	if (use_magic) {
		uint32_t marker = 0;
		const Error error = read_exact(reinterpret_cast<uint8_t *>(&marker), sizeof(marker));
		if (error != OK) { return fail(error); }
		if (p_base->is_big_endian()) { marker = BSWAP32(marker); }
		if (marker != ENCRYPTED_HEADER_MAGIC) { return fail(ERR_FILE_UNRECOGNIZED); }
	}
	uint8_t expected_hash[16];
	Error error = read_exact(expected_hash, sizeof(expected_hash));
	if (error != OK) { return fail(error); }
	error = read_exact(reinterpret_cast<uint8_t *>(&length), sizeof(length));
	if (error != OK) { return fail(error); }
	if (p_base->is_big_endian()) { length = BSWAP64(length); }
	error = iv.resize(16);
	if (error != OK) { return fail(error); }
	error = read_exact(iv.ptrw(), 16);
	if (error != OK) { return fail(error); }
	base = p_base->get_position();
	if (base != start + header_size || base > source_length) {
		return fail(ERR_FILE_CORRUPT);
	}
	const uint64_t available = source_length - base;
	const uint64_t padding = (16 - length % 16) % 16;
	if (length > available || padding > available - length) {
		return fail(ERR_FILE_CORRUPT);
	}
	const uint64_t stored_length = length + padding;
	if (stored_length > uint64_t(INT64_MAX) || stored_length > SIZE_MAX) {
		return fail(ERR_FILE_CORRUPT);
	}
	error = data.resize(stored_length);
	if (error != OK) { return fail(error); }
	if (stored_length > 0) {
		error = read_exact(data.ptrw(), stored_length);
		if (error != OK) { return fail(error); }
		CryptoCore::AESContext context;
		error = context.set_encode_key(key.ptr(), 256);
		if (error != OK) { return fail(error); }
		error = context.decrypt_cfb(stored_length, iv.ptrw(), data.ptrw(), data.ptrw());
		if (error != OK) { return fail(error); }
	}
	error = data.resize(length);
	if (error != OK) { return fail(error); }
	uint8_t actual_hash[16];
	error = CryptoCore::md5(data.ptr(), data.size(), actual_hash);
	if (error != OK) { return fail(error); }
	if (memcmp(actual_hash, expected_hash, sizeof(actual_hash)) != 0) {
		return fail(ERR_FILE_CORRUPT);
	}
	file = p_base;
	return OK;
}

Error FileAccessEncrypted::open_and_parse_password(Ref<FileAccess> p_base, const String &p_key, Mode p_mode) {
	ERR_FAIL_COND_V(file.is_valid(), ERR_ALREADY_IN_USE);
	data.clear();
	iv.clear();
	key.clear();
	length = 0;
	base = 0;
	pos = 0;
	eofed = false;
	writing = false;
	String cs = p_key.md5_text();
	ERR_FAIL_COND_V(cs.length() != 32, ERR_INVALID_PARAMETER);
	Vector<uint8_t> key_md5;
	const Error allocation = key_md5.resize(32);
	ERR_FAIL_COND_V(allocation != OK, allocation);
	for (int i = 0; i < 32; i++) {
		key_md5.write[i] = cs[i];
	}

	return open_and_parse(p_base, key_md5, p_mode);
}

Error FileAccessEncrypted::open_internal(const String &p_path, int p_mode_flags) {
	return OK;
}

void FileAccessEncrypted::_close() {
	if (file.is_null()) {
		return;
	}

	if (writing) {
		LocalVector<uint8_t> compressed;
		uint64_t len = data.size();
		if (len % 16) {
			len += 16 - (len % 16);
		}

		unsigned char hash[16];
		ERR_FAIL_COND(CryptoCore::md5(data.ptr(), data.size(), hash) != OK); // Bug?

		compressed.resize(len);
		if (!data.is_empty()) {
			memcpy(compressed.ptr(), data.ptr(), data.size());
		}
		if (len > uint64_t(data.size())) {
			memset(compressed.ptr() + data.size(), 0, len - data.size());
		}

		CryptoCore::AESContext ctx;
		ctx.set_encode_key(key.ptrw(), 256);

		if (use_magic) {
			file->store_32(ENCRYPTED_HEADER_MAGIC);
		}

		file->store_buffer(hash, 16);
		file->store_64(data.size());
		file->store_buffer(iv.ptr(), 16);

		if (len > 0) {
			ctx.encrypt_cfb(len, iv.ptrw(), compressed.ptr(), compressed.ptr());
			file->store_buffer(compressed.ptr(), compressed.size());
		}
		data.clear();
	}

	file.unref();
}

bool FileAccessEncrypted::is_open() const {
	return file.is_valid();
}

String FileAccessEncrypted::get_path() const {
	if (file.is_valid()) {
		return file->get_path();
	} else {
		return "";
	}
}

String FileAccessEncrypted::get_path_absolute() const {
	if (file.is_valid()) {
		return file->get_path_absolute();
	} else {
		return "";
	}
}

void FileAccessEncrypted::seek(uint64_t p_position) {
	if (p_position > get_length()) {
		p_position = get_length();
	}

	pos = p_position;
	eofed = false;
}

void FileAccessEncrypted::seek_end(int64_t p_position) {
	seek(get_length() + p_position);
}

uint64_t FileAccessEncrypted::get_position() const {
	return pos;
}

uint64_t FileAccessEncrypted::get_length() const {
	return data.size();
}

bool FileAccessEncrypted::eof_reached() const {
	return eofed;
}

uint64_t FileAccessEncrypted::get_buffer(uint8_t *p_dst, uint64_t p_length) const {
	ERR_FAIL_COND_V_MSG(writing, -1, "File has not been opened in read mode.");

	if (!p_length) {
		return 0;
	}

	ERR_FAIL_NULL_V(p_dst, -1);

	uint64_t to_copy = MIN(p_length, get_length() - pos);

	if (to_copy > 0) {
		memcpy(p_dst, data.ptr() + pos, to_copy);
	}
	pos += to_copy;

	if (to_copy < p_length) {
		eofed = true;
	}

	return to_copy;
}

Error FileAccessEncrypted::get_error() const {
	return eofed ? ERR_FILE_EOF : OK;
}

bool FileAccessEncrypted::store_buffer(const uint8_t *p_src, uint64_t p_length) {
	ERR_FAIL_COND_V_MSG(!writing, false, "File has not been opened in write mode.");

	if (!p_length) {
		return true;
	}

	ERR_FAIL_NULL_V(p_src, false);

	if (pos + p_length >= get_length()) {
		ERR_FAIL_COND_V(data.resize(pos + p_length) != OK, false);
	}

	memcpy(data.ptrw() + pos, p_src, p_length);
	pos += p_length;

	return true;
}

void FileAccessEncrypted::flush() {
	ERR_FAIL_COND_MSG(!writing, "File has not been opened in write mode.");

	// encrypted files keep data in memory till close()
}

bool FileAccessEncrypted::file_exists(const String &p_name) {
	Ref<FileAccess> fa = FileAccess::open(p_name, FileAccess::READ);
	if (fa.is_null()) {
		return false;
	}
	return true;
}

uint64_t FileAccessEncrypted::_get_modified_time(const String &p_file) {
	if (file.is_valid()) {
		return file->get_modified_time(p_file);
	} else {
		return 0;
	}
}

uint64_t FileAccessEncrypted::_get_access_time(const String &p_file) {
	if (file.is_valid()) {
		return file->get_access_time(p_file);
	} else {
		return 0;
	}
}

int64_t FileAccessEncrypted::_get_size(const String &p_file) {
	if (file.is_valid()) {
		return file->get_size(p_file);
	} else {
		return -1;
	}
}

BitField<FileAccess::UnixPermissionFlags> FileAccessEncrypted::_get_unix_permissions(const String &p_file) {
	if (file.is_valid()) {
		return file->_get_unix_permissions(p_file);
	}
	return 0;
}

Error FileAccessEncrypted::_set_unix_permissions(const String &p_file, BitField<FileAccess::UnixPermissionFlags> p_permissions) {
	if (file.is_valid()) {
		return file->_set_unix_permissions(p_file, p_permissions);
	}
	return FAILED;
}

bool FileAccessEncrypted::_get_hidden_attribute(const String &p_file) {
	if (file.is_valid()) {
		return file->_get_hidden_attribute(p_file);
	}
	return false;
}

Error FileAccessEncrypted::_set_hidden_attribute(const String &p_file, bool p_hidden) {
	if (file.is_valid()) {
		return file->_set_hidden_attribute(p_file, p_hidden);
	}
	return FAILED;
}

bool FileAccessEncrypted::_get_read_only_attribute(const String &p_file) {
	if (file.is_valid()) {
		return file->_get_read_only_attribute(p_file);
	}
	return false;
}

Error FileAccessEncrypted::_set_read_only_attribute(const String &p_file, bool p_ro) {
	if (file.is_valid()) {
		return file->_set_read_only_attribute(p_file, p_ro);
	}
	return FAILED;
}

void FileAccessEncrypted::close() {
	_close();
}

FileAccessEncrypted::~FileAccessEncrypted() {
	_close();
}
