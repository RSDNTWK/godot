/**************************************************************************/
/*  test_http_metadata.cpp                                                */
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

TEST_FORCE_LINK(test_http_metadata)

#ifndef WEB_ENABLED
#include "core/io/http_client_tcp.h"
#include "core/io/stream_peer.h"
#include "tests/core/io/allocation_test_helpers.h"

#include <cstring>

namespace TestHTTPMetadata {
constexpr int METADATA_CAP = 1 << 20;
constexpr int WORK_BUDGET = 1 << 14;
struct Quiet {
	bool enabled = CoreGlobals::print_error_enabled;
	Quiet() { CoreGlobals::print_error_enabled = false; }
	~Quiet() { CoreGlobals::print_error_enabled = enabled; }
};
class Peer : public StreamPeer {
public:
	Vector<uint8_t> input;
	int position = 0;
	int available_end = INT_MAX;
	bool eof = false;
	int reads = 0;
	void set_input(const String &p_input) { input = p_input.to_utf8_buffer(); }
	Error put_data(const uint8_t *, int) override { return OK; }
	Error put_partial_data(const uint8_t *, int p_bytes, int &r_sent) override {
		r_sent = p_bytes;
		return OK;
	}
	Error get_data(uint8_t *p_buffer, int p_bytes) override {
		int received = 0;
		const Error err = get_partial_data(p_buffer, p_bytes, received);
		return err != OK ? err : received == p_bytes ? OK
													 : ERR_UNAVAILABLE;
	}
	Error get_partial_data(uint8_t *p_buffer, int p_bytes, int &r_received) override {
		reads++;
		r_received = MIN(p_bytes, MAX(0, MIN(available_end, int(input.size())) - position));
		if (r_received > 0) {
			memcpy(p_buffer, input.ptr() + position, r_received);
			position += r_received;
		}
		return r_received == 0 && eof && position >= input.size() ? ERR_FILE_EOF : OK;
	}
	int get_available_bytes() const override {
		return MAX(0, MIN(available_end, int(input.size())) - position);
	}
};
struct Connection {
	Ref<Peer> peer;
	Ref<HTTPClientTCP> client;
	Quiet quiet;
	Connection(const String &p_input, HTTPClient::Method p_method = HTTPClient::METHOD_GET) {
		peer.instantiate();
		peer->set_input(p_input);
		client.instantiate();
		client->set_connection(peer);
		CHECK(client->request(p_method, "/", Vector<String>(), nullptr, 0) == OK);
	}
	void headers() {
		for (int i = 0; i < 128 && client->get_status() == HTTPClient::STATUS_REQUESTING; i++) {
			const int before = peer->position;
			const Error error = client->poll();
			CHECK(peer->position - before <= WORK_BUDGET);
			if (error != OK) {
				CHECK(client->get_status() == HTTPClient::STATUS_CONNECTION_ERROR);
				break;
			}
		}
	}
};

static String body_string(const PackedByteArray &p_data) {
	return String::utf8(reinterpret_cast<const char *>(p_data.ptr()), p_data.size());
}

static String padded_headers(int p_bytes) {
	const String prefix = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nX-Pad: ";
	const String suffix = "\r\n\r\n";
	return prefix + String("a").repeat(p_bytes - prefix.length() - suffix.length()) + suffix;
}

TEST_CASE("[E02][HTTPMetadata] Fragmented LF and CRLF headers preserve order casing and consuming access") {
	for (const String &ending : { String("\n"), String("\r\n") }) {
		const String wire = "HTTP/1.1 200 OK" + ending + "X-Custom: Value" + ending + "Content-Length: 0" + ending + ending;
		Connection c(wire);
		c.peer->available_end = wire.length() - 1;
		CHECK(c.client->poll() == OK);
		CHECK(c.client->get_status() == HTTPClient::STATUS_REQUESTING);
		c.peer->available_end++;
		c.headers();
		CHECK(c.client->get_status() == HTTPClient::STATUS_BODY);
		CHECK(c.client->get_response_code() == 200);
		List<String> headers;
		CHECK(c.client->get_response_headers(&headers) == OK);
		REQUIRE(headers.size() == 2);
		if (headers.size() != 2) {
			return;
		}
		CHECK(headers.front()->get() == "X-Custom: Value");
		CHECK(headers.back()->get() == "Content-Length: 0");
		CHECK_FALSE(c.client->has_response());
		CHECK(c.client->read_response_body_chunk().is_empty());
		CHECK(c.client->get_status() == HTTPClient::STATUS_CONNECTED);
	}
}

TEST_CASE("[E02][HTTPMetadata] Header byte limits accept their boundary and release rejected parser state") {
	for (int size : { METADATA_CAP - 1, METADATA_CAP, METADATA_CAP + 1 }) {
		Connection c(padded_headers(size));
		c.headers();
		CHECK(c.peer->position <= METADATA_CAP);
		if (size <= METADATA_CAP) {
			CHECK(c.client->get_status() == HTTPClient::STATUS_BODY);
			CHECK(c.client->get_connection().is_valid());
			CHECK(c.peer->position == size);
		} else {
			CHECK(c.client->get_status() == HTTPClient::STATUS_CONNECTION_ERROR);
			CHECK(c.client->get_connection().is_null());
			CHECK_FALSE(c.client->has_response());
			CHECK(c.client->poll() == ERR_CONNECTION_ERROR);
		}
	}
}

TEST_CASE("[E02][HTTPMetadata] Nonblocking header work yields and resumes without exposing partial metadata") {
	Connection c(padded_headers(WORK_BUDGET * 2 + 27));
	CHECK(c.client->poll() == OK);
	CHECK(c.peer->position == WORK_BUDGET);
	CHECK(c.client->get_status() == HTTPClient::STATUS_REQUESTING);
	CHECK_FALSE(c.client->has_response());
	CHECK(c.client->poll() == OK);
	CHECK(c.peer->position == WORK_BUDGET * 2);
	CHECK(c.client->get_status() == HTTPClient::STATUS_REQUESTING);
	c.headers();
	CHECK(c.client->get_status() == HTTPClient::STATUS_BODY);
	CHECK(c.peer->position == c.peer->input.size());
}

TEST_CASE("[E02][HTTPMetadata] Fragmented trailers finish before a keepalive connection is reused") {
	const String first = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\ndata\r\n0\r\nX-Trailer: discarded\r\n\r\n";
	const String second = "HTTP/1.1 201 Created\r\nContent-Length: 3\r\n\r\nnew";
	Connection c(first + second);
	c.headers();
	PackedByteArray body = c.client->read_response_body_chunk();
	CHECK(body_string(body) == "data");
	CHECK(c.client->read_response_body_chunk().is_empty());
	c.peer->available_end = first.length() - 1;
	CHECK(c.client->read_response_body_chunk().is_empty());
	CHECK(c.client->get_status() == HTTPClient::STATUS_BODY);
	c.peer->available_end++;
	CHECK(c.client->read_response_body_chunk().is_empty());
	CHECK(c.client->get_status() == HTTPClient::STATUS_CONNECTED);
	CHECK(c.peer->position == first.length());
	c.peer->available_end = INT_MAX;
	CHECK(c.client->request(HTTPClient::METHOD_GET, "/", Vector<String>(), nullptr, 0) == OK);
	c.headers();
	CHECK(c.client->get_response_code() == 201);
	CHECK(body_string(c.client->read_response_body_chunk()) == "new");
	CHECK(c.client->get_status() == HTTPClient::STATUS_CONNECTED);
}

static String trailer_bytes(int p_bytes) {
	// Many complete short lines exercise aggregate accounting after line buffers clear.
	const String line = "X: " + String("a").repeat(120) + "\r\n";
	const int copies = MAX(0, (p_bytes - 6) / line.length());
	const int final_line = p_bytes - 2 - copies * line.length();
	return line.repeat(copies) + "X:" + String("a").repeat(final_line - 4) + "\r\n\r\n";
}

TEST_CASE("[E02][HTTPMetadata] Aggregate trailer limits survive cleared lines and fragmented calls") {
	const String prefix = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n";
	for (int size : { METADATA_CAP - 1, METADATA_CAP, METADATA_CAP + 1 }) {
		Connection c(prefix + trailer_bytes(size));
		c.headers();
		CHECK(c.client->read_response_body_chunk().is_empty()); // Zero-size chunk.
		const int trailer_start = c.peer->position;
		for (int i = 0; i < 128 && c.client->get_status() == HTTPClient::STATUS_BODY; i++) {
			const int before = c.peer->position;
			CHECK(c.client->read_response_body_chunk().is_empty());
			CHECK(c.peer->position - before <= WORK_BUDGET);
		}
		CHECK(c.peer->position - trailer_start <= METADATA_CAP);
		if (size <= METADATA_CAP) {
			CHECK(c.client->get_status() == HTTPClient::STATUS_CONNECTED);
			CHECK(c.peer->position - trailer_start == size);
			CHECK(c.client->get_connection().is_valid());
		} else {
			CHECK(c.client->get_status() == HTTPClient::STATUS_CONNECTION_ERROR);
			CHECK(c.client->get_connection().is_null());
		}
	}
}

TEST_CASE("[E02][HTTPMetadata] Rejected chunk lengths stop before payload allocation or further reads") {
	const String prefix = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
	const String too_large = String::num_uint64((uint64_t(1) << 24) + 1, 16);
	for (const String &length : { String("2z"), too_large, String("1").repeat(33) }) {
		const String record = length + "\r\n";
		Connection c(prefix + record + "body bytes must remain unread");
		c.headers();
#ifdef B01_ALLOCATION_TESTS
		TestAllocation::begin_failure(size_t(1) << 24, SIZE_MAX, 0);
#endif
		CHECK(c.client->read_response_body_chunk().is_empty());
#ifdef B01_ALLOCATION_TESTS
		TestAllocation::end_failure();
		CHECK(TestAllocation::failures() == 0);
#endif
		CHECK(c.peer->position <= prefix.length() + record.length());
		CHECK(c.client->get_status() == HTTPClient::STATUS_CONNECTION_ERROR);
		CHECK(c.client->get_connection().is_null());
	}
}

TEST_CASE("[E02][HTTPMetadata] HEAD fixed length and EOF-delimited ordinary bodies remain usable") {
	{
		Connection c("HTTP/1.1 200 OK\r\nContent-Length: 999\r\n\r\n", HTTPClient::METHOD_HEAD);
		c.headers();
		CHECK(c.client->get_response_body_length() == 0);
		CHECK(c.client->read_response_body_chunk().is_empty());
		CHECK(c.client->get_status() == HTTPClient::STATUS_CONNECTED);
	}
	{
		Connection c("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc");
		c.headers();
		CHECK(body_string(c.client->read_response_body_chunk()) == "abc");
		CHECK(c.client->get_status() == HTTPClient::STATUS_CONNECTED);
	}
	{
		Connection c("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nabc");
		c.peer->eof = true;
		c.headers();
		CHECK(body_string(c.client->read_response_body_chunk()) == "abc");
		CHECK(c.client->get_status() == HTTPClient::STATUS_DISCONNECTED);
	}
}

TEST_CASE("[E02][HTTPMetadata] Blocking mode retains ordinary complete-read behavior") {
	Connection c(padded_headers(WORK_BUDGET + 100));
	c.client->set_blocking_mode(true);
	CHECK(c.client->poll() == OK);
	CHECK(c.client->get_status() == HTTPClient::STATUS_BODY);
	CHECK(c.peer->position == WORK_BUDGET + 100);
	CHECK(c.client->read_response_body_chunk().is_empty());
	CHECK(c.client->get_status() == HTTPClient::STATUS_CONNECTED);
}

#ifdef B01_ALLOCATION_TESTS
TEST_CASE("[E02][HTTPMetadata] Header buffer allocation failure closes before accepting a partial response") {
	Connection c("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
	const size_t extent = TestAllocation::allocation_size([]() {
		Vector<uint8_t> bytes;
		CHECK(bytes.resize(1) == OK);
	});
	{
		TestAllocation::Failure failure(extent);
		CHECK(c.client->poll() == ERR_CONNECTION_ERROR);
		CHECK(TestAllocation::failures() > 0);
	}
	CHECK(c.client->get_status() == HTTPClient::STATUS_CONNECTION_ERROR);
	CHECK(c.client->get_connection().is_null());
	CHECK_FALSE(c.client->has_response());
}
#endif
} // namespace TestHTTPMetadata
#endif
