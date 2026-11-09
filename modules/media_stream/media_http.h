/**************************************************************************/
/*  media_http.h                                                        */
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

#include "core/io/http_client.h"
#include "core/io/ip_address.h"
#include "core/templates/safe_refcount.h"

// Worker-thread transport shared by native media decoding and URL resolution.
// Every redirect/range/secondary resource gets a new validated, pinned connection.
class MediaHTTP {
	const SafeFlag *cancel;
	const SafeFlag *secondary_cancel;
	Ref<HTTPClient> client;
	String current_url;
	Dictionary headers;
	PackedByteArray pending;
	int pending_offset = 0;
	int64_t offset = 0;
	int64_t size = -1;
	int64_t remaining = -1;
	bool ended = false;
	bool failed = false;
	bool resumable = false;
	String entity_validator;
	uint64_t deadline = 0;
	uint64_t absolute_deadline = 0;
	int response_status = 0;
	int64_t range_chunk_bytes = 0;
	bool interrupted() const;
	Error connect(const String &p_host, int p_port);
	Error open_internal(const String &p_url, const Dictionary &p_headers, int64_t p_offset, const String &p_post, const String &p_if_range, uint64_t p_deadline);
	Error resume();

public:
	explicit MediaHTTP(const SafeFlag *p_cancel = nullptr, const SafeFlag *p_secondary_cancel = nullptr, uint64_t p_absolute_deadline = 0) : cancel(p_cancel), secondary_cancel(p_secondary_cancel), absolute_deadline(p_absolute_deadline) {}
	static bool public_address(const IPAddress &p_address);
	static bool valid_url(const String &p_url);
	static String resolve_url(const String &p_base, const String &p_target);
	// Optional bounded ranges for media CDNs which throttle open-ended GETs.
	// Zero preserves ordinary request behavior. Redirect/entity validation stays.
	void set_range_chunk_size(int64_t p_bytes);
	Error open(const String &p_url, const Dictionary &p_headers = Dictionary(), int64_t p_offset = 0, const String &p_post = String());
	int read(uint8_t *p_buffer, int p_size);
	Error seek(int64_t p_offset);
	int64_t get_size() const { return size; }
	int64_t get_position() const { return offset; }
	String get_url() const { return current_url; }
	int get_response_status() const { return response_status; }
	void close();
};
