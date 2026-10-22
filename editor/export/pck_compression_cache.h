/**************************************************************************/
/*  pck_compression_cache.h                                               */
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

#pragma once

#include "core/io/pck_lzma.h"
#include "core/string/ustring.h"

// A disposable, bounded cache of compressed plaintext, before normal export encryption.
class PCKCompressionCache {
public:
	enum Result { MISS, COMPRESSED, RAW };

private:
	struct Entry {
		String path;
		uint64_t size = 0;
		uint64_t modified = 0;
		bool operator<(const Entry &p_other) const { return modified < p_other.modified; }
	};
	String directory;
	uint64_t limit = 0;
	uint64_t disk_bytes = 0;
	Vector<Entry> entries;
	bool available = false;
	void _remove(const String &p_path);
	bool _make_room(uint64_t p_bytes);

public:
	explicit PCKCompressionCache(const String &p_directory, uint64_t p_limit = 1024ULL * 1024 * 1024);
	bool is_available() const { return available; }
	static String make_key(const Vector<uint8_t> &p_input, const PCKLzmaOptions &p_options);
	Result load(const String &p_key, const Vector<uint8_t> &p_input, Vector<uint8_t> &r_compressed);
	void store(const String &p_key, const Vector<uint8_t> &p_input, const Vector<uint8_t> &p_candidate);
};
