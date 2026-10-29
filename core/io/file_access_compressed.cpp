/**************************************************************************/
/*  file_access_compressed.cpp                                            */
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

#include "file_access_compressed.h"

#include "core/math/math_funcs_binary.h"

void FileAccessCompressed::configure(const String &p_magic, Compression::Mode p_mode, uint32_t p_block_size) {
	magic = p_magic.ascii().get_data();
	magic = (magic + "    ").substr(0, 4);

	cmode = p_mode;
	block_size = p_block_size;
}

Error FileAccessCompressed::_load_read_block(uint32_t p_block) const {
	if (read_error != OK) {
		return read_error;
	}
	const ReadBlock &block = read_blocks[p_block];
	f->seek(block.offset);
	if (f->get_position() != block.offset) {
		read_error = ERR_FILE_CANT_READ;
		return read_error;
	}
	const uint64_t actual = f->get_buffer(comp_buffer.ptrw(), block.csize);
	if (actual != block.csize) {
		const Error error = f->get_error();
		read_error = error != OK && error != ERR_FILE_EOF ? error : ERR_FILE_CORRUPT;
		return read_error;
	}
	const uint64_t offset = uint64_t(p_block) * block_size;
	const uint32_t expected = MIN(uint64_t(block_size), read_total - offset);
	const int64_t decoded = Compression::decompress(buffer.ptrw(), expected, comp_buffer.ptr(), block.csize, cmode);
	if (decoded != expected) {
		read_error = ERR_FILE_CORRUPT;
		return read_error;
	}
	read_block = p_block;
	read_block_size = expected;
	read_pos = 0;
	return OK;
}

Error FileAccessCompressed::open_after_magic(Ref<FileAccess> p_base) {
	_close();
	writing = false;
	read_error = OK;
	read_blocks.clear();
	ERR_FAIL_COND_V(p_base.is_null() || !p_base->is_open(), ERR_INVALID_PARAMETER);
	f = p_base;
	auto fail = [&](Error p_error) {
		comp_buffer.clear();
		buffer.clear();
		read_blocks.clear();
		read_ptr = nullptr;
		f.unref();
		read_error = p_error;
		return p_error;
	};
	const uint64_t header_start = f->get_position();
	const uint64_t file_length = f->get_length();
	if (header_start > file_length || file_length - header_start < 12) {
		return fail(ERR_FILE_CORRUPT);
	}
	cmode = (Compression::Mode)f->get_32();
	block_size = f->get_32();
	read_total = f->get_32();
	if (f->get_position() != header_start + 12 || cmode < Compression::MODE_FASTLZ || cmode > Compression::MODE_LZMA2 || block_size == 0) {
		return fail(ERR_FILE_CORRUPT);
	}
	const uint64_t count = read_total / block_size + 1;
	const uint64_t table_start = f->get_position();
	if (count > UINT32_MAX || count > (file_length - table_start) / 4) {
		return fail(ERR_FILE_CORRUPT);
	}
	const Error table_allocation = read_blocks.resize(count);
	if (table_allocation != OK) {
		return fail(table_allocation);
	}
	uint64_t offset = table_start + count * 4;
	uint32_t max_size = 0;
	for (uint64_t i = 0; i < count; i++) {
		ReadBlock &block = read_blocks.write[i];
		block.offset = offset;
		block.csize = f->get_32();
		if (offset > file_length || block.csize > file_length - offset) {
			return fail(ERR_FILE_CORRUPT);
		}
		offset += block.csize;
		max_size = MAX(max_size, block.csize);
	}
	if (f->get_position() != table_start + count * 4) {
		return fail(ERR_FILE_CORRUPT);
	}
	Error allocation = comp_buffer.resize(max_size);
	if (allocation != OK) {
		return fail(allocation);
	}
	allocation = buffer.resize(MIN(uint64_t(block_size), read_total));
	if (allocation != OK) {
		return fail(allocation);
	}
	read_ptr = buffer.ptrw();
	read_block_count = count;
	read_block = 0;
	read_pos = 0;
	read_eof = false;
	at_end = read_total == 0;
	const Error decoded = _load_read_block(0);
	return decoded == OK ? OK : fail(decoded);
}

Error FileAccessCompressed::open_internal(const String &p_path, int p_mode_flags) {
	ERR_FAIL_COND_V(p_mode_flags == READ_WRITE, ERR_UNAVAILABLE);
	_close();
	ERR_FAIL_COND_V((p_mode_flags & WRITE) && block_size == 0, ERR_INVALID_PARAMETER);

	Error err;
	f = FileAccess::open(p_path, p_mode_flags, &err);
	if (err != OK) {
		//not openable
		f.unref();
		return err;
	}

	if (p_mode_flags & WRITE) {
		read_error = OK;
		buffer.clear();
		writing = true;
		write_pos = 0;
		write_buffer_size = 256;
		buffer.resize(256);
		write_max = 0;
		write_ptr = buffer.ptrw();

		//don't store anything else unless it's done saving!
	} else {
		char rmagic[5];
		if (f->get_buffer((uint8_t *)rmagic, 4) != 4) {
			f.unref();
			read_error = ERR_FILE_CORRUPT;
			return read_error;
		}
		rmagic[4] = 0;
		err = ERR_FILE_UNRECOGNIZED;
		if (magic != rmagic || (err = open_after_magic(f)) != OK) {
			f.unref();
			return err;
		}
	}

	return OK;
}

void FileAccessCompressed::_close() {
	if (f.is_null()) {
		return;
	}

	if (writing) {
		//save block table and all compressed blocks

		CharString mgc = magic.utf8();
		f->store_buffer((const uint8_t *)mgc.get_data(), mgc.length()); //write header 4
		f->store_32(cmode); //write compression mode 4
		f->store_32(block_size); //write block size 4
		f->store_32(uint32_t(write_max)); //max amount of data written 4
		uint32_t bc = (write_max / block_size) + 1;

		for (uint32_t i = 0; i < bc; i++) {
			f->store_32(0); //compressed sizes, will update later
		}

		uint32_t last_block_size = write_max % block_size;

		// Temporary buffer for compressed data blocks.
		LocalVector<uint8_t> temp_cblock;
		temp_cblock.resize(Compression::get_max_compressed_buffer_size(bc == 1 ? last_block_size : block_size, cmode));
		uint8_t *temp_cblock_ptr = temp_cblock.ptr();

		// Compress and store the blocks.
		LocalVector<uint32_t> block_sizes;
		for (uint32_t i = 0; i < bc; i++) {
			uint32_t bl = i == (bc - 1) ? last_block_size : block_size;
			uint8_t *bp = &write_ptr[i * block_size];

			const int64_t compressed_size = Compression::compress(temp_cblock_ptr, bp, bl, cmode);
			ERR_FAIL_COND_MSG(compressed_size < 0, "FileAccessCompressed: Error compressing data.");

			f->store_buffer(temp_cblock_ptr, (uint64_t)compressed_size);
			block_sizes.push_back(compressed_size);
		}

		f->seek(16); //ok write block sizes
		for (uint32_t i = 0; i < bc; i++) {
			f->store_32(block_sizes[i]);
		}
		f->seek_end();
		f->store_buffer((const uint8_t *)mgc.get_data(), mgc.length()); //magic at the end too
	} else {
		comp_buffer.clear();
		read_blocks.clear();
	}
	buffer.clear();
	f.unref();
	writing = false;
	read_ptr = nullptr;
	write_ptr = nullptr;
	read_error = OK;
	at_end = false;
	read_eof = false;
}

bool FileAccessCompressed::is_open() const {
	return f.is_valid();
}

String FileAccessCompressed::get_path() const {
	if (f.is_valid()) {
		return f->get_path();
	} else {
		return "";
	}
}

String FileAccessCompressed::get_path_absolute() const {
	if (f.is_valid()) {
		return f->get_path_absolute();
	} else {
		return "";
	}
}

void FileAccessCompressed::seek(uint64_t p_position) {
	ERR_FAIL_COND_MSG(f.is_null(), "File must be opened before use.");
	if (writing) {
		ERR_FAIL_COND(p_position > write_max);
		write_pos = p_position;
		return;
	}
	if (read_error != OK) {
		return;
	}
	ERR_FAIL_COND(p_position > read_total);
	if (p_position == read_total) {
		at_end = true;
		read_eof = false;
		return;
	}
	const uint32_t block = p_position / block_size;
	if (block != read_block && _load_read_block(block) != OK) {
		return;
	}
	read_pos = p_position % block_size;
	at_end = false;
	read_eof = false;
}

void FileAccessCompressed::seek_end(int64_t p_position) {
	ERR_FAIL_COND_MSG(f.is_null(), "File must be opened before use.");
	if (writing) {
		seek(write_max + p_position);
		return;
	}
	ERR_FAIL_COND(p_position > 0 || p_position < -int64_t(read_total));
	seek(read_total - uint64_t(-p_position));
}

uint64_t FileAccessCompressed::get_position() const {
	ERR_FAIL_COND_V_MSG(f.is_null(), 0, "File must be opened before use.");
	if (writing) {
		return write_pos;
	}
	return at_end ? read_total : uint64_t(read_block) * block_size + read_pos;
}

uint64_t FileAccessCompressed::get_length() const {
	ERR_FAIL_COND_V_MSG(f.is_null(), 0, "File must be opened before use.");
	if (writing) {
		return write_max;
	} else {
		return read_total;
	}
}

bool FileAccessCompressed::eof_reached() const {
	ERR_FAIL_COND_V_MSG(f.is_null(), false, "File must be opened before use.");
	if (writing) {
		return false;
	} else {
		return read_eof;
	}
}

uint64_t FileAccessCompressed::get_buffer(uint8_t *p_dst, uint64_t p_length) const {
	if (p_length == 0 || read_error != OK) {
		return 0;
	}
	ERR_FAIL_NULL_V(p_dst, 0);
	ERR_FAIL_COND_V_MSG(f.is_null() || writing, 0, "File has not been opened in read mode.");
	uint64_t copied = 0;
	while (copied < p_length) {
		if (at_end || uint64_t(read_block) * block_size + read_pos == read_total) {
			at_end = true;
			read_eof = true;
			break;
		}
		if (read_pos == read_block_size) {
			if (_load_read_block(read_block + 1) != OK) {
				break;
			}
		}
		const uint64_t amount = MIN(p_length - copied, read_block_size - read_pos);
		memcpy(p_dst + copied, read_ptr + read_pos, amount);
		copied += amount;
		read_pos += amount;
	}
	return copied;
}

Error FileAccessCompressed::get_error() const {
	if (read_error != OK) {
		return read_error;
	}
	if (f.is_null()) {
		return ERR_UNCONFIGURED;
	}
	return read_eof ? ERR_FILE_EOF : OK;
}

void FileAccessCompressed::flush() {
	ERR_FAIL_COND_MSG(f.is_null(), "File must be opened before use.");
	ERR_FAIL_COND_MSG(!writing, "File has not been opened in write mode.");

	// compressed files keep data in memory till close()
}

bool FileAccessCompressed::store_buffer(const uint8_t *p_src, uint64_t p_length) {
	ERR_FAIL_COND_V_MSG(f.is_null(), false, "File must be opened before use.");
	ERR_FAIL_COND_V_MSG(!writing, false, "File has not been opened in write mode.");

	if (write_pos + (p_length) > write_max) {
		write_max = write_pos + (p_length);
	}
	if (write_max > write_buffer_size) {
		write_buffer_size = Math::next_power_of_2(write_max);
		ERR_FAIL_COND_V(buffer.resize(write_buffer_size) != OK, false);
		write_ptr = buffer.ptrw();
	}

	if (p_length) {
		memcpy(write_ptr + write_pos, p_src, p_length);
	}

	write_pos += p_length;
	return true;
}

bool FileAccessCompressed::file_exists(const String &p_name) {
	Ref<FileAccess> fa = FileAccess::open(p_name, FileAccess::READ);
	if (fa.is_null()) {
		return false;
	}
	return true;
}

uint64_t FileAccessCompressed::_get_modified_time(const String &p_file) {
	if (f.is_valid()) {
		return f->get_modified_time(p_file);
	} else {
		return 0;
	}
}

uint64_t FileAccessCompressed::_get_access_time(const String &p_file) {
	if (f.is_valid()) {
		return f->get_access_time(p_file);
	} else {
		return 0;
	}
}

int64_t FileAccessCompressed::_get_size(const String &p_file) {
	if (f.is_valid()) {
		return f->get_size(p_file);
	} else {
		return -1;
	}
}

BitField<FileAccess::UnixPermissionFlags> FileAccessCompressed::_get_unix_permissions(const String &p_file) {
	if (f.is_valid()) {
		return f->_get_unix_permissions(p_file);
	}
	return 0;
}

Error FileAccessCompressed::_set_unix_permissions(const String &p_file, BitField<FileAccess::UnixPermissionFlags> p_permissions) {
	if (f.is_valid()) {
		return f->_set_unix_permissions(p_file, p_permissions);
	}
	return FAILED;
}

bool FileAccessCompressed::_get_hidden_attribute(const String &p_file) {
	if (f.is_valid()) {
		return f->_get_hidden_attribute(p_file);
	}
	return false;
}

Error FileAccessCompressed::_set_hidden_attribute(const String &p_file, bool p_hidden) {
	if (f.is_valid()) {
		return f->_set_hidden_attribute(p_file, p_hidden);
	}
	return FAILED;
}

bool FileAccessCompressed::_get_read_only_attribute(const String &p_file) {
	if (f.is_valid()) {
		return f->_get_read_only_attribute(p_file);
	}
	return false;
}

Error FileAccessCompressed::_set_read_only_attribute(const String &p_file, bool p_ro) {
	if (f.is_valid()) {
		return f->_set_read_only_attribute(p_file, p_ro);
	}
	return FAILED;
}

void FileAccessCompressed::close() {
	_close();
}

FileAccessCompressed::~FileAccessCompressed() {
	_close();
}
