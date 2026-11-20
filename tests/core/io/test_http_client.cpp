/**************************************************************************/
/*  test_http_client.cpp                                                  */
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

TEST_FORCE_LINK(test_http_client)

#include "core/io/http_client.h"
#include "core/io/stream_peer.h"

#include "modules/modules_enabled.gen.h" // For mbedtls.

namespace TestHTTPClient {

TEST_CASE("[HTTPClient] Instantiation") {
	Ref<HTTPClient> client = HTTPClient::create();
	CHECK_MESSAGE(client.is_valid(), "A HTTP Client created should not be a null pointer");
}

TEST_CASE("[HTTPClient] query_string_from_dict") {
	Ref<HTTPClient> client = HTTPClient::create();
	Dictionary empty_dict;
	String empty_query = client->query_string_from_dict(empty_dict);
	CHECK_MESSAGE(empty_query.is_empty(), "A empty dictionary should return a empty string");

	Dictionary dict1;
	dict1["key"] = "value";
	String single_key = client->query_string_from_dict(dict1);
	CHECK_MESSAGE(single_key == "key=value", "The query should return key=value for every string in the dictionary");

	// Check Dictionary with multiple values of different types.
	Dictionary dict2;
	dict2["key1"] = "value";
	dict2["key2"] = 123;
	Array values = { 1, 2, 3 };
	dict2["key3"] = values;
	dict2["key4"] = Variant();
	String multiple_keys = client->query_string_from_dict(dict2);
	CHECK_MESSAGE(multiple_keys == "key1=value&key2=123&key3=1&key3=2&key3=3&key4",
			"The query should return key=value for every string in the dictionary. Pairs should be separated by &, arrays should be have a query for every element, and variants should have empty values");
}

TEST_CASE("[HTTPClient] verify_headers") {
	Ref<HTTPClient> client = HTTPClient::create();
	Vector<String> headers = { "Accept: text/html", "Content-Type: application/json", "Authorization: Bearer abc123" };

	Error err = client->verify_headers(headers);
	CHECK_MESSAGE(err == OK, "Expected OK for valid headers");

	ERR_PRINT_OFF;
	Vector<String> empty_header = { "" };
	err = client->verify_headers(empty_header);
	CHECK_MESSAGE(err == ERR_INVALID_PARAMETER, "Expected ERR_INVALID_PARAMETER for empty header");

	Vector<String> invalid_header = { "InvalidHeader", "Header: " };
	err = client->verify_headers(invalid_header);
	CHECK_MESSAGE(err == ERR_INVALID_PARAMETER, "Expected ERR_INVALID_PARAMETER for header with no colon");

	Vector<String> invalid_header_b = { ":", "Header: " };
	err = client->verify_headers(invalid_header_b);
	CHECK_MESSAGE(err == ERR_INVALID_PARAMETER, "Expected ERR_INVALID_PARAMETER for header with colon in first position");
	ERR_PRINT_ON;
}

#if defined(MODULE_MBEDTLS_ENABLED) || defined(WEB_ENABLED)
TEST_CASE("[HTTPClient] connect_to_host") {
	Ref<HTTPClient> client = HTTPClient::create();
	String host = "https://www.example.com";
	int port = 443;
	Ref<TLSOptions> tls_options;

	// Connect to host.
	Error err = client->connect_to_host(host, port, tls_options);
	CHECK_MESSAGE(err == OK, "Expected OK for successful connection");
}
#endif // defined(MODULE_MBEDTLS_ENABLED) || defined(WEB_ENABLED)

#ifndef WEB_ENABLED
// Writes and reads are independent, like a connected TCP stream.
class HTTPResponseFixture : public StreamPeerBuffer {
public:
	Error put_data(const uint8_t *p_data, int p_bytes) override { return OK; }
	Error put_partial_data(const uint8_t *p_data, int p_bytes, int &r_sent) override {
		r_sent = p_bytes;
		return OK;
	}
};

TEST_CASE("[HTTPClient] Native request target forms") {
	Ref<HTTPClient> client = HTTPClient::create();
	Ref<HTTPResponseFixture> stream;
	stream.instantiate();
	const Vector<String> headers = { "Host: example.com" };
	for (const String &target : { String("/media/file%20one.mp4?start=2&sig=a+b%2F"), String("https://example.com:8443/media/file?q=a/b?c"), String("http://[::1]/media") }) {
		client->close();
		client->set_connection(stream);
		CHECK(client->request(HTTPClient::METHOD_GET, target, headers, nullptr, 0) == OK);
	}
	client->close();
	client->set_connection(stream);
	CHECK(client->request(HTTPClient::METHOD_OPTIONS, "*", headers, nullptr, 0) == OK);
	client->close();
	client->set_connection(stream);
	CHECK(client->request(HTTPClient::METHOD_CONNECT, "[::1]:443", headers, nullptr, 0) == OK);

	client->close();
	client->set_connection(stream);
	ERR_PRINT_OFF;
	for (const String &target : { String("/media/a b"), String("/media/a\tb"), String("/media/%2"), String("/media#page"), String("relative"), String("https:///media") }) {
		CHECK(client->request(HTTPClient::METHOD_GET, target, headers, nullptr, 0) == ERR_INVALID_PARAMETER);
		CHECK(client->get_status() == HTTPClient::STATUS_CONNECTED);
	}
	CHECK(client->request(HTTPClient::METHOD_GET, "*", headers, nullptr, 0) == ERR_INVALID_PARAMETER);
	CHECK(client->request(HTTPClient::METHOD_CONNECT, "example.com:65536", headers, nullptr, 0) == ERR_INVALID_PARAMETER);
	ERR_PRINT_ON;
}

TEST_CASE("[HTTPClient] Native response headers are bounded and incremental") {
	for (bool blocking : { false, true }) {
		INFO(blocking);
		Ref<HTTPClient> client = HTTPClient::create();
		Ref<HTTPResponseFixture> stream;
		stream.instantiate();
		stream->set_data_array(("HTTP/1.1 200 OK\r\nX-Test: " + String("a").repeat(6 * 1024) + "\r\nContent-Length: 3\r\n\r\nabc").to_utf8_buffer());
		client->set_connection(stream);
		client->set_blocking_mode(blocking);
		REQUIRE(client->request(HTTPClient::METHOD_GET, "/", { "Host: example.com" }, nullptr, 0) == OK);
		REQUIRE(client->poll() == OK);
		CHECK(client->get_status() == HTTPClient::STATUS_REQUESTING);
		CHECK(stream->get_position() == 4 * 1024);
		REQUIRE(client->poll() == OK);
		CHECK(client->get_response_code() == 200);
		REQUIRE(client->get_status() == HTTPClient::STATUS_BODY);
		CHECK(client->read_response_body_chunk() == String("abc").to_utf8_buffer());
	}

	Ref<HTTPClient> client = HTTPClient::create();
	Ref<HTTPResponseFixture> stream;
	stream.instantiate();
	stream->set_data_array(("HTTP/1.1 200 OK\r\nX-Test: " + String("a").repeat(64 * 1024) + "\r\n\r\n").to_utf8_buffer());
	client->set_connection(stream);
	REQUIRE(client->request(HTTPClient::METHOD_GET, "/", { "Host: example.com" }, nullptr, 0) == OK);
	for (int poll = 0; poll < 15; poll++) {
		REQUIRE(client->poll() == OK);
	}
	CHECK(client->poll() == ERR_INVALID_DATA);
	CHECK(client->get_status() == HTTPClient::STATUS_CONNECTION_ERROR);
	CHECK_FALSE(client->has_response());
	CHECK(stream->get_position() == 64 * 1024);
}
#endif // WEB_ENABLED

} // namespace TestHTTPClient
