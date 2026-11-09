/**************************************************************************/
/*  media_http.cpp                                                      */
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

#include "media_http.h"

#include "core/io/ip.h"
#include "core/os/os.h"

#include <cstring>

bool MediaHTTP::public_address(const IPAddress &p_address) {
	if (!p_address.is_valid() || p_address.is_wildcard()) {
		return false;
	}
	if (p_address.is_ipv4()) {
		const uint8_t *b = p_address.get_ipv4();
		if (b[0] == 0 || b[0] == 10 || b[0] == 127 || b[0] >= 224 ||
				(b[0] == 100 && b[1] >= 64 && b[1] <= 127) ||
				(b[0] == 169 && b[1] == 254) || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) ||
				(b[0] == 192 && (b[1] == 168 || (b[1] == 0 && b[2] == 0) || (b[1] == 0 && b[2] == 2))) ||
				(b[0] == 198 && (b[1] == 18 || b[1] == 19 || (b[1] == 51 && b[2] == 100))) ||
				(b[0] == 203 && b[1] == 0 && b[2] == 113)) {
			return false;
		}
		return true;
	}
	const uint8_t *b = p_address.get_ipv6();
	// The well-known NAT64 /96 prefix is usable on IPv6-only Android networks,
	// provided its embedded IPv4 destination passes the same public-address check.
	static const uint8_t nat64[12] = { 0, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0 };
	if (memcmp(b, nat64, 12) == 0) {
		IPAddress embedded;
		embedded.set_ipv4(b + 12);
		return public_address(embedded);
	}
	// Globally routed unicast only; exclude documentation and transition ranges.
	return (b[0] & 0xe0) == 0x20 && !(b[0] == 0x20 && b[1] == 0x02) &&
			!(b[0] == 0x20 && b[1] == 0x01 && b[2] < 2) &&
			!(b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) &&
			!(b[0] == 0x3f && (b[1] & 0xf0) == 0xf0);
}

bool MediaHTTP::valid_url(const String &p_url, bool p_allow_http) {
	if (p_url.length() > 32768 || (!p_url.begins_with("https://") && !(p_allow_http && p_url.begins_with("http://"))) || p_url.contains("@") || p_url.contains("\\")) {
		return false;
	}
	for (int i = 0; i < p_url.length(); i++) {
		if (p_url[i] <= 32 || p_url[i] == 127) {
			return false;
		}
	}
	String scheme, host, path, fragment;
	int port;
	// parse_url rejects explicit ports outside 1..65535. Port zero here means
	// that the URL omitted its port; the caller supplies the scheme's default.
	return p_url.parse_url(scheme, host, port, path, fragment) == OK && !host.is_empty() && fragment.is_empty();
}

String MediaHTTP::resolve_url(const String &p_base, const String &p_target) {
	if (p_target.contains("://")) {
		return p_target;
	}
	String scheme, host, path, fragment;
	int port;
	if (p_base.parse_url(scheme, host, port, path, fragment) != OK) {
		return String();
	}
	if (p_target.begins_with("//")) {
		return scheme + p_target.trim_prefix("//");
	}
	String origin = scheme + (host.contains(":") ? "[" + host + "]" : host) + (port ? ":" + itos(port) : String());
	if (p_target.begins_with("/")) {
		return origin + p_target;
	}
	String base_path = path.get_slice("?", 0);
	if (p_target.begins_with("?")) {
		return origin + base_path + p_target;
	}
	return origin + base_path.substr(0, base_path.rfind_char('/') + 1) + p_target;
}

bool MediaHTTP::interrupted() const {
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	return (cancel && cancel->is_set()) || (secondary_cancel && secondary_cancel->is_set()) || now >= deadline || (absolute_deadline && now >= absolute_deadline);
}

Error MediaHTTP::connect(const String &p_host, int p_port, bool p_tls) {
	Vector<IPAddress> candidates;
	if (IPAddress::is_valid_ip_address(p_host)) {
		IPAddress literal(p_host);
		if (!public_address(literal)) {
			return ERR_UNAUTHORIZED;
		}
		candidates.push_back(literal);
	} else {
		IP *ip = IP::get_singleton();
		IP::ResolverID id = ip->resolve_hostname_queue_item(p_host);
		if (id == IP::RESOLVER_INVALID_ID) {
			return ERR_CANT_RESOLVE;
		}
		while (!interrupted() && ip->get_resolve_item_status(id) == IP::RESOLVER_STATUS_WAITING) {
			OS::get_singleton()->delay_usec(2000);
		}
		if (interrupted() || ip->get_resolve_item_status(id) != IP::RESOLVER_STATUS_DONE) {
			ip->erase_resolve_item(id);
			return interrupted() ? ERR_TIMEOUT : ERR_CANT_RESOLVE;
		}
		Array addresses = ip->get_resolve_item_addresses(id);
		ip->erase_resolve_item(id);
		if (interrupted()) {
			return ERR_TIMEOUT;
		}
		if (addresses.is_empty() || addresses.size() > 64) {
			return ERR_CANT_RESOLVE;
		}
		for (int i = 0; i < addresses.size(); i++) {
			IPAddress address{ String(addresses[i]) };
			if (!public_address(address)) {
				return ERR_UNAUTHORIZED;
			}
			if (address.is_ipv4()) {
				candidates.insert(0, address);
			} else {
				candidates.push_back(address);
			}
		}
	}
	Error last_error = ERR_CANT_CONNECT;
	for (const IPAddress &address : candidates) {
		if (interrupted()) {
			return ERR_TIMEOUT;
		}
		if (client.is_valid()) {
			client->close();
		}
		client = HTTPClient::create();
		client->set_blocking_mode(false);
		client->set_read_chunk_size(65536);
		// Both schemes pin the vetted address. HTTPS retains hostname validation/SNI.
		last_error = client->connect_to_host(String(address), p_port, p_tls ? TLSOptions::client(Ref<X509Certificate>(), p_host) : Ref<TLSOptions>());
		if (last_error != OK) {
			continue;
		}
		uint64_t attempt_deadline = MIN(deadline, OS::get_singleton()->get_ticks_usec() + 5000000);
		while (!interrupted() && OS::get_singleton()->get_ticks_usec() < attempt_deadline) {
			last_error = client->poll();
			if (last_error != OK) {
				break;
			}
			HTTPClient::Status status = client->get_status();
			if (status == HTTPClient::STATUS_CONNECTED) {
				return OK;
			}
			if (status != HTTPClient::STATUS_CONNECTING && status != HTTPClient::STATUS_RESOLVING) {
				break;
			}
			OS::get_singleton()->delay_usec(2000);
		}
		last_error = ERR_CANT_CONNECT;
	}
	return last_error;
}

Error MediaHTTP::open(const String &p_url, const Dictionary &p_headers, int64_t p_offset, const String &p_post) {
	return open_internal(p_url, p_headers, p_offset, p_post, String(), OS::get_singleton()->get_ticks_usec() + 15000000);
}

void MediaHTTP::set_range_chunk_size(int64_t p_bytes) {
	range_chunk_bytes = CLAMP(p_bytes, int64_t(0), int64_t(10 * 1024 * 1024));
}

Error MediaHTTP::open_internal(const String &p_url, const Dictionary &p_headers, int64_t p_offset, const String &p_post, const String &p_if_range, uint64_t p_deadline) {
	if (p_offset < 0 || p_offset == INT64_MAX) {
		return ERR_INVALID_PARAMETER;
	}
	close();
	response_status = 0;
	headers = p_headers;
	String url = p_url;
	String post = p_post;
	String initial_scheme, initial_host, initial_path, initial_fragment;
	int initial_port;
	if (!valid_url(p_url, allow_http) || p_url.parse_url(initial_scheme, initial_host, initial_port, initial_path, initial_fragment) != OK) {
		return ERR_UNAUTHORIZED;
	}
	initial_port = initial_port ? initial_port : (initial_scheme == "https://" ? 443 : 80);
	bool require_https = initial_scheme == "https://";
	deadline = p_deadline;
	for (int redirect = 0; redirect < 6 && !interrupted(); redirect++) {
		if (!valid_url(url, allow_http && !require_https)) {
			return ERR_UNAUTHORIZED;
		}
		String scheme, host, path, fragment;
		int port;
		url.parse_url(scheme, host, port, path, fragment);
		const bool tls = scheme == "https://";
		require_https = require_https || tls;
		port = port ? port : (tls ? 443 : 80);
		const bool same_origin = scheme == initial_scheme && host == initial_host && port == initial_port;
		if (!same_origin) {
			if (!post.is_empty()) {
				return ERR_UNAUTHORIZED;
			}
			for (const String &name : { String("X-YouTube-Client-Name"), String("X-YouTube-Client-Version"), String("X-Goog-Visitor-Id") }) {
				headers.erase(name);
			}
		}
		Error error = connect(host, port, tls);
		if (error != OK) {
			return error;
		}
		Vector<String> request_headers;
		request_headers.push_back("Host: " + (host.contains(":") ? "[" + host + "]" : host) + (port == (tls ? 443 : 80) ? String() : ":" + itos(port)));
		request_headers.push_back("Accept-Encoding: identity");
		// API client metadata is restricted to the original origin. Authentication
		// headers are never forwarded by this transport.
		for (const String &name : { String("User-Agent"), String("Referer"), String("Accept"), String("Origin"), String("X-YouTube-Client-Name"), String("X-YouTube-Client-Version"), String("X-Goog-Visitor-Id") }) {
			if (name.begins_with("X-") && !same_origin) {
				continue;
			}
			String value = headers.get(name, String());
			bool valid = true;
			for (int i = 0; i < value.length(); i++) {
				if (value[i] < 32 || value[i] == 127) {
					valid = false;
					break;
				}
			}
			if (!value.is_empty() && valid) {
				request_headers.push_back(name + ": " + value);
			}
		}
		if (!post.is_empty()) {
			request_headers.push_back("Content-Type: application/json");
		}
		const int64_t range_end = post.is_empty() && range_chunk_bytes > 0 ? p_offset + MIN(range_chunk_bytes - 1, INT64_MAX - 1 - p_offset) : -1;
		if (p_offset > 0 || range_end >= 0) {
			request_headers.push_back("Range: bytes=" + itos(p_offset) + "-" + (range_end >= 0 ? itos(range_end) : String()));
			if (!p_if_range.is_empty()) {
				request_headers.push_back("If-Range: " + p_if_range);
			}
		}
		CharString encoded = post.utf8();
		error = client->request(post.is_empty() ? HTTPClient::METHOD_GET : HTTPClient::METHOD_POST, path.is_empty() ? "/" : path, request_headers, reinterpret_cast<const uint8_t *>(encoded.get_data()), encoded.length());
		if (error != OK) {
			return error;
		}
		while (!interrupted() && !client->has_response()) {
			if (client->poll() != OK || client->get_status() == HTTPClient::STATUS_DISCONNECTED || client->get_status() == HTTPClient::STATUS_CONNECTION_ERROR) {
				return ERR_CONNECTION_ERROR;
			}
			OS::get_singleton()->delay_usec(2000);
		}
		if (interrupted()) {
			return ERR_TIMEOUT;
		}
		int code = client->get_response_code();
		response_status = code;
		List<String> response_headers;
		client->get_response_headers(&response_headers);
		String location;
		String range;
		String etag;
		String modified;
		for (const String &header : response_headers) {
			int colon = header.find_char(':');
			if (colon < 0) {
				continue;
			}
			String name = header.substr(0, colon).to_lower();
			String value = header.substr(colon + 1).strip_edges();
			if (name == "location") {
				location = value;
			} else if (name == "content-range") {
				range = value;
			} else if (name == "etag" && !value.begins_with("W/")) {
				etag = value;
			} else if (name == "last-modified") {
				modified = value;
			} else if (name == "content-encoding" && value != "identity") {
				return ERR_UNAVAILABLE;
			}
		}
		if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
			if (location.is_empty()) {
				return ERR_INVALID_DATA;
			}
			url = resolve_url(url, location);
			if (code == 303 || code == 301 || code == 302) {
				post = String();
			}
			client->close();
			continue;
		}
		if (code != 200 && code != 206) {
			return code == 403 ? ERR_UNAUTHORIZED : ERR_CANT_CONNECT;
		}
		if (p_offset > 0 && code != 206) {
			return ERR_UNAVAILABLE;
		}
		remaining = client->get_response_body_length();
		size = remaining;
		if (code == 206) {
			if (!range.begins_with("bytes ") || range.get_slice_count("/") != 2 || range.substr(6).get_slice("/", 0).get_slice_count("-") != 2) {
				return ERR_INVALID_DATA;
			}
			String first = range.substr(6).get_slice("-", 0);
			String last = range.get_slice("-", 1).get_slice("/", 0);
			String total = range.get_slice("/", 1);
			if (!first.is_valid_int() || !last.is_valid_int() || first.length() > 19 || last.length() > 19 || first.to_int() != p_offset || last.to_int() < p_offset || last.to_int() == INT64_MAX || (range_end >= 0 && last.to_int() > range_end) ||
					(total != "*" && (!total.is_valid_int() || total.length() > 19 || total.to_int() <= last.to_int())) ||
					(remaining >= 0 && remaining != last.to_int() - p_offset + 1)) {
				return ERR_INVALID_DATA;
			}
			size = total == "*" ? -1 : total.to_int();
		}
		offset = p_offset;
		current_url = url;
		entity_validator = etag.is_empty() ? modified : etag;
		if (entity_validator.length() > 1024) {
			return ERR_INVALID_DATA;
		}
		for (int i = 0; i < entity_validator.length(); i++) {
			if (entity_validator[i] < 32 || entity_validator[i] == 127) {
				return ERR_INVALID_DATA;
			}
		}
		resumable = post.is_empty() && size > 0;
		return OK;
	}
	return ERR_TIMEOUT;
}

Error MediaHTTP::resume() {
	const String url = current_url;
	const Dictionary request_headers = headers;
	const String validator = entity_validator;
	const int64_t expected_size = size;
	const int64_t from = offset;
	const uint64_t request_deadline = deadline;
	Error error = open_internal(url, request_headers, from, String(), validator, request_deadline);
	if (error == OK && (size != expected_size || (!validator.is_empty() && entity_validator != validator))) {
		error = ERR_INVALID_DATA;
	}
	return error;
}

int MediaHTTP::read(uint8_t *p_buffer, int p_size) {
	if (failed) {
		return -1;
	}
	if (p_size <= 0 || ended) {
		return 0;
	}
	deadline = OS::get_singleton()->get_ticks_usec() + 15000000;
	int reopens = 0;
	while (!interrupted() && client.is_valid()) {
		if (pending_offset < pending.size()) {
			int amount = MIN(p_size, pending.size() - pending_offset);
			memcpy(p_buffer, pending.ptr() + pending_offset, amount);
			pending_offset += amount;
			offset += amount;
			return amount;
		}
		if (size >= 0 && offset == size) {
			ended = true;
			return 0;
		}
		const Error poll_error = client->poll();
		HTTPClient::Status status = client->get_status();
		if (poll_error == OK && status == HTTPClient::STATUS_BODY) {
			pending = client->read_response_body_chunk();
			pending_offset = 0;
			if (remaining >= 0) {
				remaining -= pending.size();
			}
		} else {
			const bool response_complete = poll_error == OK && (status == HTTPClient::STATUS_CONNECTED || status == HTTPClient::STATUS_DISCONNECTED) && remaining <= 0;
			if (response_complete && (size < 0 || offset == size)) {
				ended = true;
				return 0;
			}
			// A completed partial range or an interrupted known-length GET is
			// not file EOF. Resume before handing a truncated packet to demuxing.
			// Every reopen repeats the normal pinned HTTPS/range validation.
			if (!interrupted() && resumable && offset < size && reopens++ < 3 && resume() == OK) {
				continue;
			}
			failed = true;
			return -1;
		}
		OS::get_singleton()->delay_usec(2000);
	}
	failed = true;
	return -1;
}

Error MediaHTTP::seek(int64_t p_offset) {
	if (p_offset < 0 || (size >= 0 && p_offset > size)) {
		return ERR_INVALID_PARAMETER;
	}
	if (p_offset == offset && !failed) {
		return OK;
	}
	if (size >= 0 && p_offset == size) {
		if (client.is_valid()) {
			client->close();
		}
		pending.clear();
		pending_offset = 0;
		offset = p_offset;
		ended = true;
		failed = false;
		return OK;
	}
	String url = current_url;
	Dictionary request_headers = headers;
	return open(url, request_headers, p_offset);
}

void MediaHTTP::close() {
	if (client.is_valid()) {
		client->close();
		client.unref();
	}
	pending.clear();
	pending_offset = 0;
	ended = false;
	failed = false;
	resumable = false;
	entity_validator = String();
	size = -1;
	remaining = -1;
	offset = 0;
}
