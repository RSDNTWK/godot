/**************************************************************************/
/*  file_access_patched.cpp                                               */
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

#include "file_access_patched.h"

#include "core/io/delta_encoding.h"
#include "core/io/file_access_pack.h"
#include "core/os/os.h"

static Error _read_complete_patch_input(const Ref<FileAccess> &p_file, Vector<uint8_t> &r_data) {
	r_data.clear();
	ERR_FAIL_COND_V(p_file.is_null() || !p_file->is_open(), ERR_FILE_CANT_OPEN);
	const uint64_t size = p_file->get_length();
	ERR_FAIL_COND_V(size > uint64_t(INT64_MAX) || size > SIZE_MAX, ERR_FILE_CORRUPT);
	p_file->seek(0);
	const Error seek_error = p_file->get_error();
	ERR_FAIL_COND_V(seek_error != OK && seek_error != ERR_FILE_EOF, seek_error);
	ERR_FAIL_COND_V(p_file->get_position() != 0, ERR_FILE_CANT_READ);
	const Error allocation_error = r_data.resize(size);
	ERR_FAIL_COND_V(allocation_error != OK, allocation_error);
	if (size > 0) {
		const uint64_t read = p_file->get_buffer(r_data.ptrw(), size);
		const Error error = p_file->get_error();
		if (read != size || (error != OK && error != ERR_FILE_EOF)) {
			r_data.clear();
			return error != OK && error != ERR_FILE_EOF ? error : ERR_FILE_CORRUPT;
		}
	}
	return OK;
}

Error FileAccessPatched::_apply_patch() const {
	ERR_FAIL_COND_V(!is_open(), FAILED);

	String path = old_file->get_path();
	Vector<PackedData::PackedFile> delta_patches = PackedData::get_singleton()->get_delta_patches(path);
	Vector<uint8_t> old_file_data;
	const Error base_error = _read_complete_patch_input(old_file, old_file_data);
	ERR_FAIL_COND_V(base_error != OK, base_error);

	for (int i = 0; i < delta_patches.size(); ++i) {
		const PackedData::PackedFile &delta_patch = delta_patches[i];
		ERR_FAIL_COND_V(delta_patch.bundle, FAILED);

		Error err = OK;

		uint64_t total_usec_start = OS::get_singleton()->get_ticks_usec();
		uint64_t io_usec_start = OS::get_singleton()->get_ticks_usec();

		// Use the packed-entry reader directly: the provider would recursively
		// wrap this payload in the same patch chain again.
		Ref<FileAccess> patch_file(memnew(FileAccessPack(path, delta_patch, decryption_key)));
		if (!patch_file->is_open()) {
			err = patch_file->get_error();
			return err != OK ? err : ERR_FILE_CANT_OPEN;
		}
		Vector<uint8_t> patch_data;
		err = _read_complete_patch_input(patch_file, patch_data);
		ERR_FAIL_COND_V(err != OK, err);
		ERR_FAIL_COND_V(patch_data.is_empty(), ERR_FILE_CANT_READ);

		uint64_t io_usec_end = OS::get_singleton()->get_ticks_usec();
		uint64_t decode_usec_start = OS::get_singleton()->get_ticks_usec();

		Vector<uint8_t> new_file_data;
		err = DeltaEncoding::decode_delta(old_file_data, patch_data, new_file_data);
		ERR_FAIL_COND_V_MSG(err != OK, err, vformat("Failed to apply delta patch (%d of %d) to \"%s\".", i + 1, delta_patches.size(), path));

		uint64_t decode_usec_end = OS::get_singleton()->get_ticks_usec();

		old_file_data = new_file_data;

		uint64_t total_usec_end = OS::get_singleton()->get_ticks_usec();

		print_verbose(vformat(U"Applied delta patch to \"%s\" from \"%s\" in %d μs (%d μs I/O, %d μs decoding).", path, delta_patch.pack.get_file(), total_usec_end - total_usec_start, io_usec_end - io_usec_start, decode_usec_end - decode_usec_start));
	}

	const uint64_t logical_size = old_file_data.size();
	patched_file_data = old_file_data;
	if (logical_size == 0) {
		// FileAccessMemory requires a valid address even for a zero-length
		// stream. Keep its backing alive without changing logical content.
		const Error allocation_error = patched_file_data.resize(1);
		ERR_FAIL_COND_V(allocation_error != OK, allocation_error);
		patched_file_data.write[0] = 0;
	}
	patched_file.instantiate();
	return patched_file->open_custom(patched_file_data.ptr(), logical_size);
}

bool FileAccessPatched::_try_apply_patch() const {
	if (last_error != OK) {
		return false;
	}

	if (patched_file.is_valid()) {
		return true;
	}

	last_error = _apply_patch();
	return last_error == OK;
}

Error FileAccessPatched::open_custom(const Ref<FileAccess> &p_old_file, const Vector<uint8_t> &p_decryption_key) {
	close();

	if (p_old_file.is_null() || !p_old_file->is_open()) {
		last_error = ERR_FILE_CANT_OPEN;
		return last_error;
	}

	old_file = p_old_file;
	decryption_key = p_decryption_key;

	return OK;
}

bool FileAccessPatched::is_open() const {
	return old_file.is_valid() && old_file->is_open();
}

void FileAccessPatched::seek(uint64_t p_position) {
	if (!_try_apply_patch()) {
		return;
	}

	patched_file->seek(p_position);
}

void FileAccessPatched::seek_end(int64_t p_position) {
	if (!_try_apply_patch()) {
		return;
	}

	patched_file->seek_end(p_position);
}

uint64_t FileAccessPatched::get_position() const {
	if (!_try_apply_patch()) {
		return 0;
	}

	return patched_file->get_position();
}

uint64_t FileAccessPatched::get_length() const {
	if (!_try_apply_patch()) {
		return 0;
	}

	return patched_file->get_length();
}

bool FileAccessPatched::eof_reached() const {
	if (!_try_apply_patch()) {
		return true;
	}

	return patched_file->eof_reached();
}

Error FileAccessPatched::get_error() const {
	if (last_error != OK) {
		return last_error;
	}

	if (patched_file.is_valid()) {
		Error inner_error = patched_file->get_error();
		if (inner_error != OK) {
			return inner_error;
		}
	}

	return last_error;
}

bool FileAccessPatched::store_buffer(const uint8_t *p_src, uint64_t p_length) {
	if (!_try_apply_patch()) {
		return false;
	}

	return patched_file->store_buffer(p_src, p_length);
}

uint64_t FileAccessPatched::get_buffer(uint8_t *p_dst, uint64_t p_length) const {
	if (!_try_apply_patch()) {
		return 0;
	}

	return patched_file->get_buffer(p_dst, p_length);
}

void FileAccessPatched::flush() {
	if (!_try_apply_patch()) {
		return;
	}

	patched_file->flush();
}

void FileAccessPatched::close() {
	old_file = Ref<FileAccess>();
	decryption_key.clear();
	patched_file = Ref<FileAccessMemory>();
	patched_file_data.clear();
	last_error = OK;
}

bool FileAccessPatched::file_exists(const String &p_name) {
	ERR_FAIL_COND_V(old_file.is_null(), false);
	return old_file->file_exists(p_name);
}
