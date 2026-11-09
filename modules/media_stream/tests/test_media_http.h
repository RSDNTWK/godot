/**************************************************************************/
/*  test_media_http.h                                                   */
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

#include "media_stream_test_utils.h"

#include "core/object/class_db.h"
#include "core/os/os.h"
#include "tests/test_macros.h"

#include "modules/media_stream/media_http.h"

#include <cstring>

namespace TestMediaHTTP {

struct ScriptedReply {
	int code = 200;
	int64_t length = 8;
	String range;
	String body;
	String validator = "\"version-1\"";
	String location;
	bool disconnect = false;
};

struct ScriptedHTTP {
	Vector<ScriptedReply> replies;
	Vector<Vector<String>> requests;
	Vector<HTTPClient::Method> methods;
	Vector<int> ports;
	Vector<String> tls_names;
	Vector<bool> unsafe_tls;
	int connections = 0;
};

// Exercise the real transport's URL, range, entity and retry handling without
// opening sockets. The existing HTTPClient factory is restored after each case.
class ScriptedClient : public HTTPClient {
	GDCLASS(ScriptedClient, HTTPClient);
	static inline ScriptedHTTP *fixture = nullptr;
	ScriptedReply reply;
	Status status = STATUS_DISCONNECTED;
	bool sent = false;
	bool response = false;
	static HTTPClient *create_scripted(bool p_notify) { return static_cast<HTTPClient *>(ClassDB::creator<ScriptedClient>(p_notify)); }

protected:
	static void _bind_methods() {}

public:
	using Factory = HTTPClient *(*)(bool);
	static Factory install(ScriptedHTTP *p_fixture) {
		Factory previous = _create;
		fixture = p_fixture;
		_create = create_scripted;
		return previous;
	}
	static void restore(Factory p_previous) {
		_create = p_previous;
		fixture = nullptr;
	}
	Error connect_to_host(const String &, int p_port, Ref<TLSOptions> p_tls) override {
		fixture->ports.push_back(p_port);
		fixture->tls_names.push_back(p_tls.is_valid() ? p_tls->get_common_name_override() : String());
		fixture->unsafe_tls.push_back(p_tls.is_null() || p_tls->is_unsafe_client());
		reply = fixture->replies[MIN(fixture->connections++, fixture->replies.size() - 1)];
		status = STATUS_CONNECTED;
		return OK;
	}
	Error request(Method p_method, const String &, const Vector<String> &p_headers, const uint8_t *, int) override {
		fixture->requests.push_back(p_headers);
		fixture->methods.push_back(p_method);
		status = STATUS_BODY;
		response = true;
		return OK;
	}
	void set_connection(const Ref<StreamPeer> &) override {}
	Ref<StreamPeer> get_connection() const override { return Ref<StreamPeer>(); }
	void close() override { status = STATUS_DISCONNECTED; }
	Status get_status() const override { return status; }
	bool has_response() const override { return response; }
	bool is_response_chunked() const override { return false; }
	int get_response_code() const override { return reply.code; }
	int64_t get_response_body_length() const override { return reply.length; }
	Error get_response_headers(List<String> *r_headers) override {
		if (!reply.location.is_empty()) {
			r_headers->push_back("Location: " + reply.location);
		}
		if (!reply.range.is_empty()) {
			r_headers->push_back("Content-Range: " + reply.range);
		}
		if (!reply.validator.is_empty()) {
			r_headers->push_back("ETag: " + reply.validator);
		}
		return OK;
	}
	PackedByteArray read_response_body_chunk() override {
		PackedByteArray bytes;
		if (!sent) {
			CharString body = reply.body.utf8();
			bytes.resize(body.length());
			if (body.length()) {
				memcpy(bytes.ptrw(), body.get_data(), body.length());
			}
			sent = true;
		}
		return bytes;
	}
	void set_blocking_mode(bool) override {}
	bool is_blocking_mode_enabled() const override { return false; }
	void set_read_chunk_size(int) override {}
	int get_read_chunk_size() const override { return 65536; }
	Error poll() override {
		if (status == STATUS_BODY && sent) {
			status = reply.disconnect ? STATUS_CONNECTION_ERROR : STATUS_CONNECTED;
		}
		return status == STATUS_CONNECTION_ERROR ? ERR_CONNECTION_ERROR : OK;
	}
};

struct ScriptedFactory {
	ScriptedClient::Factory previous;
	ScriptedFactory(ScriptedHTTP &p_fixture) : previous(ScriptedClient::install(&p_fixture)) {}
	~ScriptedFactory() { ScriptedClient::restore(previous); }
};

TEST_CASE("[MediaStream][NativeStreaming] Partial responses and interrupted GET bodies resume at the consumed byte without duplicating data") {
	for (bool disconnect : { false, true }) {
		ScriptedHTTP fixture;
		ScriptedReply first;
		first.code = disconnect ? 200 : 206;
		first.length = disconnect ? 8 : 4;
		first.range = disconnect ? String() : String("bytes 0-3/8");
		first.body = "abcd";
		first.disconnect = disconnect;
		fixture.replies.push_back(first);
		ScriptedReply second;
		second.code = 206;
		second.length = 4;
		second.range = "bytes 4-7/8";
		second.body = "efgh";
		fixture.replies.push_back(second);
		ScriptedFactory factory(fixture);
		SafeFlag cancel;
		MediaHTTP http(&cancel);
		REQUIRE(http.open("https://8.8.8.8/media") == OK);
		uint8_t bytes[16] = {};
		REQUIRE(http.read(bytes, 1) == 1);
		REQUIRE(http.read(bytes + 1, 15) == 3);
		REQUIRE(http.read(bytes + 4, 12) == 4);
		CHECK(memcmp(bytes, "abcdefgh", 8) == 0);
		CHECK(http.get_position() == 8);
		CHECK(http.get_size() == 8);
		CHECK(http.read(bytes, 16) == 0);
		REQUIRE(fixture.requests.size() == 2);
		CHECK(fixture.requests[1].has("Range: bytes=4-"));
		CHECK(fixture.requests[1].has("If-Range: \"version-1\""));
	}
}

TEST_CASE("[MediaStream][NativeStreaming] Bounded media ranges include the first chunk and resume without gaps") {
	ScriptedHTTP fixture;
	for (int i = 0; i < 2; i++) {
		ScriptedReply reply;
		reply.code = 206;
		reply.length = 4;
		reply.range = i == 0 ? "bytes 0-3/8" : "bytes 4-7/8";
		reply.body = i == 0 ? "abcd" : "efgh";
		fixture.replies.push_back(reply);
	}
	ScriptedFactory factory(fixture);
	SafeFlag cancel;
	MediaHTTP http(&cancel);
	http.set_range_chunk_size(4);
	REQUIRE(http.open("https://8.8.8.8/media") == OK);
	uint8_t bytes[8] = {};
	REQUIRE(http.read(bytes, 8) == 4);
	REQUIRE(http.read(bytes + 4, 4) == 4);
	CHECK(memcmp(bytes, "abcdefgh", 8) == 0);
	CHECK(http.read(bytes, 8) == 0);
	REQUIRE(fixture.requests.size() == 2);
	CHECK(fixture.requests[0].has("Range: bytes=0-3"));
	CHECK(fixture.requests[1].has("Range: bytes=4-7"));
	CHECK(fixture.requests[1].has("If-Range: \"version-1\""));
}

TEST_CASE("[MediaStream][NativeStreaming] Bounded range responses cannot exceed the requested end") {
	ScriptedHTTP fixture;
	ScriptedReply reply;
	reply.code = 206;
	reply.length = 8;
	reply.range = "bytes 0-7/8";
	reply.body = "abcdefgh";
	fixture.replies.push_back(reply);
	ScriptedFactory factory(fixture);
	SafeFlag cancel;
	MediaHTTP http(&cancel);
	http.set_range_chunk_size(4);
	CHECK(http.open("https://8.8.8.8/media") == ERR_INVALID_DATA);
}

TEST_CASE("[MediaStream][NativeStreaming] Initial ignored ranges retain ordinary GET compatibility and POST is unchanged") {
	for (bool post : { false, true }) {
		ScriptedHTTP fixture;
		ScriptedReply reply;
		reply.body = "abcdefgh";
		fixture.replies.push_back(reply);
		ScriptedFactory factory(fixture);
		SafeFlag cancel;
		MediaHTTP http(&cancel);
		http.set_range_chunk_size(64 * 1024 * 1024);
		REQUIRE(http.open("https://8.8.8.8/media", Dictionary(), 0, post ? String("{}") : String()) == OK);
		uint8_t bytes[8];
		REQUIRE(http.read(bytes, 8) == 8);
		CHECK(memcmp(bytes, "abcdefgh", 8) == 0);
		CHECK(http.read(bytes, 8) == 0);
		REQUIRE(fixture.requests.size() == 1);
		CHECK(fixture.requests[0].has("Range: bytes=0-10485759") == !post);
	}
}

TEST_CASE("[MediaStream][NativeStreaming] Resumed media rejects a changed entity, total length, offset or ignored range") {
	for (int invalid = 0; invalid < 4; invalid++) {
		ScriptedHTTP fixture;
		ScriptedReply first;
		first.body = "abcd";
		first.disconnect = true;
		fixture.replies.push_back(first);
		ScriptedReply second;
		second.code = invalid == 3 ? 200 : 206;
		second.length = 4;
		second.range = invalid == 1 ? "bytes 4-7/9" : (invalid == 2 ? "bytes 3-6/8" : "bytes 4-7/8");
		second.body = "efgh";
		if (invalid == 0) {
			second.validator = "\"changed\"";
		}
		fixture.replies.push_back(second);
		ScriptedFactory factory(fixture);
		SafeFlag cancel;
		MediaHTTP http(&cancel);
		REQUIRE(http.open("https://8.8.8.8/media") == OK);
		uint8_t bytes[16];
		REQUIRE(http.read(bytes, 16) == 4);
		CHECK(http.read(bytes, 16) == -1);
		CHECK(http.read(bytes, 16) == -1);
		CHECK(fixture.requests.size() == 2);
	}
}

TEST_CASE("[MediaStream][NativeStreaming] Empty reconnects are bounded and cancellation or POST responses never resume as GET") {
	for (int mode = 0; mode < 3; mode++) {
		ScriptedHTTP fixture;
		ScriptedReply reply;
		reply.body = mode == 1 ? "abcd" : String();
		reply.disconnect = true;
		fixture.replies.push_back(reply);
		ScriptedFactory factory(fixture);
		SafeFlag cancel;
		MediaHTTP http(&cancel);
		REQUIRE(http.open("https://8.8.8.8/media", Dictionary(), 0, mode == 2 ? String("{}") : String()) == OK);
		uint8_t bytes[16];
		if (mode == 1) {
			REQUIRE(http.read(bytes, 16) == 4);
			cancel.set();
		}
		CHECK(http.read(bytes, 16) == -1);
		CHECK(fixture.requests.size() == (mode == 0 ? 4 : 1));
		CHECK(fixture.methods[0] == (mode == 2 ? HTTPClient::METHOD_POST : HTTPClient::METHOD_GET));
	}
}

TEST_CASE("[MediaStream][Network] HTTPS range reopening preserves data" * doctest::skip()) {
	String url = OS::get_singleton()->get_environment("GODOT_MEDIA_TEST_URL");
	REQUIRE(!url.is_empty());
	if (url.is_empty()) {
		return;
	}
	TestMediaStream::load_certificates();
	SafeFlag cancelled;
	MediaHTTP http(&cancelled);
	Error error = http.open(url);
	INFO(http.get_response_status());
	REQUIRE(error == OK);
	if (error != OK) {
		return;
	}
	CHECK(http.get_size() > 131072);
	uint8_t first[1024];
	uint8_t second[1024];
	int count = http.read(first, sizeof(first));
	REQUIRE(count > 0);
	if (count <= 0) {
		return;
	}
	CHECK(http.seek(65536) == OK);
	CHECK(http.read(second, sizeof(second)) > 0);
	CHECK(http.seek(0) == OK);
	int repeated = http.read(second, count);
	CHECK(repeated == count);
	if (repeated == count) {
		CHECK(memcmp(first, second, count) == 0);
	}
	CHECK(http.seek(http.get_size()) == OK);
	CHECK(http.read(second, sizeof(second)) == 0);
}

TEST_CASE("[MediaStream][NativeStreaming] Destination addresses must be public unicast") {
	CHECK(MediaHTTP::public_address(IPAddress("8.8.8.8")));
	CHECK(MediaHTTP::public_address(IPAddress("2606:4700:4700::1111")));
	CHECK(MediaHTTP::public_address(IPAddress("64:ff9b::808:808")));
	CHECK_FALSE(MediaHTTP::public_address(IPAddress("64:ff9b::7f00:1")));
	for (const String &address : { String("127.0.0.1"), String("10.0.0.1"), String("172.16.1.1"), String("192.168.1.1"), String("169.254.1.1"), String("100.64.0.1"), String("192.0.2.1"), String("224.0.0.1"), String("::1"), String("fe80::1"), String("fc00::1"), String("2001:db8::1"), String("::ffff:127.0.0.1") }) {
		CHECK_FALSE(MediaHTTP::public_address(IPAddress(address)));
	}
}

TEST_CASE("[MediaStream][NativeStreaming] Redirect and secondary-resource URLs share HTTPS validation") {
	CHECK(MediaHTTP::resolve_url("https://example.com/a/index.m3u8", "segment.ts") == "https://example.com/a/segment.ts");
	CHECK(MediaHTTP::resolve_url("https://example.com/a/index.m3u8", "/b/segment.ts") == "https://example.com/b/segment.ts");
	CHECK(MediaHTTP::resolve_url("https://example.com/a/index.m3u8", "//cdn.example.com/segment.ts") == "https://cdn.example.com/segment.ts");
	CHECK(MediaHTTP::resolve_url("https://example.com/a/index.m3u8?old=1", "?new=2") == "https://example.com/a/index.m3u8?new=2");
	CHECK(MediaHTTP::valid_url("https://example.com/video?token=a%2Bb"));
	CHECK_FALSE(MediaHTTP::valid_url("http://example.com/video"));
	CHECK_FALSE(MediaHTTP::valid_url("file:///video"));
	CHECK_FALSE(MediaHTTP::valid_url("https://user@example.com/video"));
	CHECK(MediaHTTP::valid_url("https://example.com:8080/video"));
	CHECK(MediaHTTP::valid_url("https://example.com:65535/video"));
	CHECK(MediaHTTP::resolve_url("https://example.com:8443/a/index.m3u8", "segment.ts") == "https://example.com:8443/a/segment.ts");
	CHECK(MediaHTTP::resolve_url("https://[2606:4700:4700::1111]:8443/a/index.m3u8", "/segment.ts") == "https://[2606:4700:4700::1111]:8443/segment.ts");
	for (const String &port : { String("0"), String("-1"), String("65536"), String("4294975739"), String("-4294967295"), String("abc"), String("") }) {
		CHECK_FALSE(MediaHTTP::valid_url("https://example.com:" + port + "/video"));
	}
	CHECK_FALSE(MediaHTTP::valid_url("https://example.com/video\r\nHeader: value"));
}

TEST_CASE("[MediaStream][NativeStreaming] HTTPS custom ports preserve TLS identity and isolate redirect origins") {
	for (const String &host : { String("8.8.8.8"), String("[2606:4700:4700::1111]") }) {
		ScriptedHTTP fixture;
		ScriptedReply first;
		first.code = 302;
		first.location = "https://" + host + ":9443/media";
		fixture.replies.push_back(first);
		ScriptedReply second;
		second.body = "abcdefgh";
		fixture.replies.push_back(second);
		ScriptedFactory factory(fixture);
		Dictionary headers;
		headers["X-YouTube-Client-Name"] = "unit-client";
		MediaHTTP http;
		REQUIRE(http.open("https://" + host + ":8443/media", headers) == OK);
		REQUIRE(fixture.ports.size() == 2);
		CHECK(fixture.ports[0] == 8443);
		CHECK(fixture.ports[1] == 9443);
		CHECK(fixture.requests[0].has("Host: " + host + ":8443"));
		CHECK(fixture.requests[1].has("Host: " + host + ":9443"));
		CHECK(fixture.requests[0].has("X-YouTube-Client-Name: unit-client"));
		CHECK_FALSE(fixture.requests[1].has("X-YouTube-Client-Name: unit-client"));
		const String tls_host = host.trim_prefix("[").trim_suffix("]");
		CHECK(fixture.tls_names[0] == tls_host);
		CHECK(fixture.tls_names[1] == tls_host);
		CHECK_FALSE(fixture.unsafe_tls[0]);
		CHECK_FALSE(fixture.unsafe_tls[1]);
	}
	SUBCASE("POST redirects cannot change port even on the same host") {
		ScriptedHTTP fixture;
		ScriptedReply reply;
		reply.code = 307;
		reply.location = "https://8.8.8.8:9443/media";
		fixture.replies.push_back(reply);
		ScriptedFactory factory(fixture);
		MediaHTTP http;
		CHECK(http.open("https://8.8.8.8:8443/media", Dictionary(), 0, "{}") == ERR_UNAUTHORIZED);
		CHECK(fixture.connections == 1);
	}
	SUBCASE("A custom port cannot admit a private redirect destination") {
		ScriptedHTTP fixture;
		ScriptedReply reply;
		reply.code = 302;
		reply.location = "https://127.0.0.1:9443/media";
		fixture.replies.push_back(reply);
		ScriptedFactory factory(fixture);
		MediaHTTP http;
		CHECK(http.open("https://8.8.8.8:8443/media") == ERR_UNAUTHORIZED);
		CHECK(fixture.connections == 1);
	}
}
} // namespace TestMediaHTTP
