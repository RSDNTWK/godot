/**************************************************************************/
/*  file_access_android.cpp                                               */
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

#include "file_access_android.h"

#include "thread_jandroid.h"

#include "core/string/print_string.h"

#include <android/asset_manager_jni.h>

AAssetManager *FileAccessAndroid::asset_manager = nullptr;
jobject FileAccessAndroid::j_asset_manager = nullptr;

String FileAccessAndroid::get_path() const {
	return path_src;
}

String FileAccessAndroid::get_path_absolute() const {
	return absolute_path;
}

Error FileAccessAndroid::open_internal(const String &p_path, int p_mode_flags) {
	_close();
	last_read_error = OK;
	len = 0;
	pos = 0;
	eof = false;

	path_src = p_path;
	String path = fix_path(p_path).simplify_path();
	absolute_path = path;
	if (path.begins_with("/")) {
		path = path.substr(1);
	} else if (path.begins_with("res://")) {
		path = path.substr(6);
	}

	ERR_FAIL_COND_V(p_mode_flags & FileAccess::WRITE, ERR_UNAVAILABLE); //can't write on android..
	asset = AAssetManager_open(asset_manager, path.utf8().get_data(), AASSET_MODE_STREAMING);
	if (!asset) {
		return ERR_CANT_OPEN;
	}
	const int64_t length = AAsset_getLength64(asset);
	if (length < 0) {
		last_read_error = ERR_FILE_CANT_READ;
		_close();
		return last_read_error;
	}
	len = length;
	pos = 0;
	eof = false;

	return OK;
}

void FileAccessAndroid::_close() {
	if (!asset) {
		return;
	}
	AAsset_close(asset);
	asset = nullptr;
}

bool FileAccessAndroid::is_open() const {
	return asset != nullptr;
}

void FileAccessAndroid::_seek(int64_t p_offset, int p_whence) {
	last_read_error = OK;
	if (!asset) {
		last_read_error = ERR_UNCONFIGURED;
		return;
	}
	const int64_t base = p_whence == SEEK_END ? int64_t(len) : 0;
	if (p_offset < -base || p_offset > INT64_MAX - base) {
		last_read_error = ERR_FILE_CANT_READ;
		return;
	}
	const int64_t expected = base + p_offset;
	const int64_t actual = AAsset_seek64(asset, p_offset, p_whence);
	if (actual < 0 || actual != expected) {
		last_read_error = ERR_FILE_CANT_READ;
		return;
	}
	pos = MIN(uint64_t(actual), len);
	eof = uint64_t(actual) > len;
}

void FileAccessAndroid::seek(uint64_t p_position) {
	if (p_position > uint64_t(INT64_MAX)) {
		last_read_error = ERR_FILE_CANT_READ;
		return;
	}
	_seek(int64_t(p_position), SEEK_SET);
}

void FileAccessAndroid::seek_end(int64_t p_position) {
	_seek(p_position, SEEK_END);
}

uint64_t FileAccessAndroid::get_position() const {
	return pos;
}

uint64_t FileAccessAndroid::get_length() const {
	return len;
}

bool FileAccessAndroid::eof_reached() const {
	return eof;
}

uint64_t FileAccessAndroid::get_buffer(uint8_t *p_dst, uint64_t p_length) const {
	if (p_length == 0) {
		return 0;
	}
	if (last_read_error != OK) {
		return 0;
	}
	if (!asset) {
		last_read_error = ERR_UNCONFIGURED;
		return 0;
	}
	if (!p_dst) {
		last_read_error = ERR_INVALID_PARAMETER;
		return 0;
	}

	uint64_t total = 0;
	while (total < p_length) {
		const int requested = int(MIN(p_length - total, uint64_t(INT_MAX)));
		const int actual = AAsset_read(asset, p_dst + total, requested);
		if (actual < 0 || actual > requested) {
			last_read_error = ERR_FILE_CANT_READ;
			break;
		}
		if (actual == 0) {
			eof = true;
			break;
		}
		total += actual;
		pos = MIN(pos + uint64_t(actual), len);
	}
	return total;
}

int64_t FileAccessAndroid::_get_size(const String &p_file) {
	// FileAccess::get_size creates an unopened backend for this query.
	FileAccessAndroid file;
	file._set_access_type(get_access_type());
	if (file.open_internal(p_file, READ) != OK) {
		return -1;
	}
	return file.get_length();
}

Error FileAccessAndroid::get_error() const {
	if (last_read_error != OK) {
		return last_read_error;
	}
	return asset ? (eof ? ERR_FILE_EOF : OK) : ERR_UNCONFIGURED;
}

void FileAccessAndroid::flush() {
	ERR_FAIL();
}

bool FileAccessAndroid::store_buffer(const uint8_t *p_src, uint64_t p_length) {
	ERR_FAIL_V(false);
}

bool FileAccessAndroid::file_exists(const String &p_path) {
	String path = fix_path(p_path).simplify_path();
	if (path.begins_with("/")) {
		path = path.substr(1);
	} else if (path.begins_with("res://")) {
		path = path.substr(6);
	}

	AAsset *at = AAssetManager_open(asset_manager, path.utf8().get_data(), AASSET_MODE_STREAMING);

	if (!at) {
		return false;
	}

	AAsset_close(at);
	return true;
}

void FileAccessAndroid::close() {
	_close();
}

FileAccessAndroid::~FileAccessAndroid() {
	_close();
}

void FileAccessAndroid::setup(jobject p_asset_manager) {
	JNIEnv *env = get_jni_env();
	j_asset_manager = env->NewGlobalRef(p_asset_manager);
	asset_manager = AAssetManager_fromJava(env, j_asset_manager);
}

void FileAccessAndroid::terminate() {
	JNIEnv *env = get_jni_env();
	ERR_FAIL_NULL(env);

	env->DeleteGlobalRef(j_asset_manager);
}
