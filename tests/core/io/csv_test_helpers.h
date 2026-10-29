/**************************************************************************/
/*  csv_test_helpers.h                                                    */
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

#include "core/io/file_access_memory.h"

namespace TestCSVReadErrors {
struct ErrorsOff {
	bool previous = CoreGlobals::print_error_enabled;
	ErrorsOff() { CoreGlobals::print_error_enabled = false; }
	~ErrorsOff() { CoreGlobals::print_error_enabled = previous; }
};

class Reader : public FileAccessMemory {
	Vector<uint8_t> bytes;
	uint64_t failure_position = UINT64_MAX;
	mutable bool ended = false;
	mutable bool failed = false;

public:
	mutable int failed_reads = 0;
	Error open_bytes(const Vector<uint8_t> &p_bytes, uint64_t p_failure_position = UINT64_MAX) {
		bytes = p_bytes;
		failure_position = p_failure_position;
		return open_custom(bytes.ptr(), bytes.size());
	}
	void set_failure_position(uint64_t p_position) {
		failure_position = p_position;
		ended = false;
		failed = false;
		failed_reads = 0;
	}
	void seek(uint64_t p_position) override {
		FileAccessMemory::seek(p_position);
		ended = false;
		failed = false;
		failed_reads = 0;
	}
	uint64_t get_buffer(uint8_t *p_dst, uint64_t p_length) const override {
		if (!p_length) {
			return 0;
		}
		if (failed || (get_position() >= failure_position && failure_position < get_length())) {
			failed = true;
			failed_reads++;
			return 0;
		}
		if (get_position() >= get_length()) {
			ended = true;
			return 0;
		}
		ended = false;
		const uint64_t read = FileAccessMemory::get_buffer(p_dst, MIN(p_length, failure_position - get_position()));
		ended = read < p_length && get_position() >= get_length();
		return read;
	}
	Error get_error() const override { return failed ? ERR_FILE_CANT_READ : ended ? ERR_FILE_EOF
																				  : OK; }
	// A finite test-only escape prevents a regressed parser from hanging the runner.
	bool eof_reached() const override { return ended || failed_reads >= 32; }
};

} // namespace TestCSVReadErrors
