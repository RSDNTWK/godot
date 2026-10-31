/**************************************************************************/
/*  test_dtls_receive.cpp                                                 */
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

#ifdef B01_ALLOCATION_TESTS
#include "tls_test_sizes.h"
#endif
#include "../packet_peer_mbed_dtls.h"
#include "tests/test_macros.h"

#include "tests/core/io/allocation_test_helpers.h"
#include <array>
#include <cstring>

namespace TestDTLSReceive {

class ScriptedUDP : public PacketPeerUDP {
	Vector<Vector<uint8_t>> incoming;
	int next = 0;
	uint8_t empty_byte = 0;

public:
	Vector<Vector<uint8_t>> outgoing;
	int reads = 0;
	int count_error = 0;
	Error read_error = OK;

	ScriptedUDP() {
		connected = true;
		packet_ip = IPAddress("127.0.0.1");
		packet_port = 12345;
	}

	void enqueue(const Vector<uint8_t> &p_packet) {
		incoming.push_back(p_packet);
	}
	int get_available_packet_count() const override {
		return count_error < 0 ? count_error : incoming.size() - next;
	}
	Error get_packet(const uint8_t **r_buffer, int &r_size) override {
		reads++;
		if (read_error != OK) {
			return read_error;
		}
		if (next == incoming.size()) {
			return ERR_UNAVAILABLE;
		}
		const Vector<uint8_t> &packet = incoming[next++];
		r_size = packet.size();
		*r_buffer = r_size ? packet.ptr() : &empty_byte;
		return OK;
	}
	Error put_packet(const uint8_t *p_buffer, int p_size) override {
		Vector<uint8_t> packet;
		Error error = packet.resize(p_size);
		if (error != OK) {
			return error;
		}
		if (p_size > 0) {
			memcpy(packet.ptrw(), p_buffer, p_size);
		}
		outgoing.push_back(packet);
		return OK;
	}
	void transfer_to(const Ref<ScriptedUDP> &p_other) {
		for (const Vector<uint8_t> &packet : outgoing) {
			p_other->enqueue(packet);
		}
		outgoing.clear();
	}
};

class DTLSProbe : public PacketPeerMbedDTLS {
public:
	int receive(uint8_t *p_destination, size_t p_capacity) {
		mbedtls_ssl_context *context = tls_ctx->get_context();
		return context->MBEDTLS_PRIVATE(f_recv)(context->MBEDTLS_PRIVATE(p_bio), p_destination, p_capacity);
	}
};

inline Ref<DTLSProbe> waiting_client(const Ref<ScriptedUDP> &p_transport) {
	Ref<DTLSProbe> client;
	client.instantiate();
	// A chain object is sufficient for installing the BIO: no remote certificate
	// is read in these callback-only tests. The full handshake below uses a real CA.
	Ref<X509CertificateMbedTLS> empty_chain;
	empty_chain.instantiate();
	REQUIRE(client->connect_to_peer(p_transport, "localhost", TLSOptions::client(empty_chain)) == OK);
	REQUIRE(client->get_status() == PacketPeerDTLS::STATUS_HANDSHAKING);
	return client;
}

void destination_capacity_and_progression() {
	Ref<ScriptedUDP> transport;
	transport.instantiate();
	Ref<DTLSProbe> client = waiting_client(transport);
	Vector<uint8_t> oversized = { 1, 2, 3, 4, 5 };
	Vector<uint8_t> exact = { 9, 8, 7, 6 };
	Vector<uint8_t> shorter = { 3, 2 };
	transport->enqueue(oversized);
	transport->enqueue(exact);
	transport->enqueue(shorter);

	std::array<uint8_t, 24> destination;
	destination.fill(0xa7);
	// Oversized cases are executed only with the corrected callback.
	CHECK(client->receive(destination.data() + 8, 4) == MBEDTLS_ERR_SSL_WANT_READ);
	CHECK(transport->reads == 1);
	CHECK(transport->get_available_packet_count() == 2);
	for (uint8_t value : destination) {
		CHECK(value == 0xa7);
	}

	CHECK(client->receive(destination.data() + 8, 4) == 4);
	for (int i = 0; i < int(destination.size()); i++) {
		CHECK(destination[i] == (i >= 8 && i < 12 ? exact[i - 8] : 0xa7));
	}
	destination.fill(0xa7);
	CHECK(client->receive(destination.data() + 8, 4) == 2);
	for (int i = 0; i < int(destination.size()); i++) {
		CHECK(destination[i] == (i >= 8 && i < 10 ? shorter[i - 8] : 0xa7));
	}
	CHECK(transport->reads == 3);
	CHECK(client->receive(destination.data() + 8, 4) == MBEDTLS_ERR_SSL_WANT_READ);
	CHECK(transport->reads == 3);
}

void retry_and_transport_errors() {
	Ref<ScriptedUDP> transport;
	transport.instantiate();
	Ref<DTLSProbe> client = waiting_client(transport);
	std::array<uint8_t, 8> destination;
	destination.fill(0x6b);
	CHECK(client->receive(destination.data(), destination.size()) == MBEDTLS_ERR_SSL_WANT_READ);
	transport->enqueue(Vector<uint8_t>{ 4, 5, 6 });
	CHECK(client->receive(destination.data(), 0) == 0);
	CHECK(client->receive(nullptr, 4) == 0);
	CHECK(transport->reads == 0);
	CHECK(transport->get_available_packet_count() == 1);

	transport->count_error = -1;
	ERR_PRINT_OFF;
	CHECK(client->receive(destination.data(), destination.size()) == MBEDTLS_ERR_SSL_INTERNAL_ERROR);
	ERR_PRINT_ON;
	CHECK(transport->reads == 0);
	transport->count_error = 0;
	transport->read_error = ERR_CANT_OPEN;
	CHECK(client->receive(destination.data(), destination.size()) == MBEDTLS_ERR_SSL_INTERNAL_ERROR);
	CHECK(transport->reads == 1);
	for (uint8_t value : destination) {
		CHECK(value == 0x6b);
	}
	transport->read_error = OK;
	CHECK(client->receive(destination.data(), destination.size()) == 3);
	CHECK(destination[0] == 4);
	CHECK(destination[1] == 5);
	CHECK(destination[2] == 6);
}

void server_handshake_and_connected_progression() {
	CryptoMbedTLS crypto;
	Ref<CryptoKey> key = crypto.generate_rsa(2048);
	REQUIRE(key.is_valid());
	Ref<X509Certificate> certificate = crypto.generate_self_signed_certificate(
			key, "CN=localhost,O=Godot DTLS test,C=GB", "20200101000000", "20371231235959");
	REQUIRE(certificate.is_valid());
	Ref<TLSOptions> server_options = TLSOptions::server(key, certificate);
	Ref<CookieContextMbedTLS> cookies;
	cookies.instantiate();
	REQUIRE(cookies->setup() == OK);

	Ref<ScriptedUDP> client_transport;
	client_transport.instantiate();
	Ref<DTLSProbe> client;
	client.instantiate();
	REQUIRE(client->connect_to_peer(client_transport, "localhost", TLSOptions::client(certificate)) == OK);
	REQUIRE(!client_transport->outgoing.is_empty());

	// Obtain a normal cookie-bearing ClientHello through the existing DTLS
	// cookie exchange. No sockets or external services are used.
	Ref<ScriptedUDP> cookie_transport;
	cookie_transport.instantiate();
	client_transport->transfer_to(cookie_transport);
	Ref<DTLSProbe> cookie_server;
	cookie_server.instantiate();
	CHECK(cookie_server->accept_peer(cookie_transport, server_options, cookies) == FAILED);
	REQUIRE(!cookie_transport->outgoing.is_empty());
	cookie_transport->transfer_to(client_transport);
	client->poll();
	REQUIRE(!client_transport->outgoing.is_empty());
	REQUIRE(client->get_status() == PacketPeerDTLS::STATUS_HANDSHAKING);

	Ref<ScriptedUDP> server_transport;
	server_transport.instantiate();
	Vector<uint8_t> oversized;
	REQUIRE(oversized.resize(65507) == OK);
	memset(oversized.ptrw(), 0x4e, oversized.size());
	server_transport->enqueue(oversized);
	client_transport->transfer_to(server_transport);
	int queued = server_transport->get_available_packet_count();
	REQUIRE(queued >= 2);
	Ref<DTLSProbe> server;
	server.instantiate();
	REQUIRE(server->accept_peer(server_transport, server_options, cookies) == OK);
	CHECK(server->get_status() == PacketPeerDTLS::STATUS_HANDSHAKING);
	CHECK(server_transport->reads == 1);
	CHECK(server_transport->get_available_packet_count() == queued - 1);

	server->poll();
	CHECK(server_transport->reads >= 2);
	REQUIRE((server->get_status() == PacketPeerDTLS::STATUS_HANDSHAKING ||
			server->get_status() == PacketPeerDTLS::STATUS_CONNECTED));
	REQUIRE(!server_transport->outgoing.is_empty());
	for (int step = 0; step < 64; step++) {
		server_transport->transfer_to(client_transport);
		client->poll();
		client_transport->transfer_to(server_transport);
		server->poll();
		if (client->get_status() == PacketPeerDTLS::STATUS_CONNECTED &&
				server->get_status() == PacketPeerDTLS::STATUS_CONNECTED) {
			break;
		}
	}
	REQUIRE(client->get_status() == PacketPeerDTLS::STATUS_CONNECTED);
	REQUIRE(server->get_status() == PacketPeerDTLS::STATUS_CONNECTED);

	const uint8_t message[] = { 3, 1, 4, 1, 5 };
	REQUIRE(client->put_packet(message, sizeof(message)) == OK);
	server_transport->enqueue(oversized);
	client_transport->transfer_to(server_transport);
	const uint8_t *received = nullptr;
	int received_size = 0;
	const int reads_before_rejection = server_transport->reads;
	CHECK(server->get_packet(&received, received_size) == OK);
	CHECK(received_size == 0);
	CHECK(server_transport->reads == reads_before_rejection + 1);
	CHECK(server_transport->get_available_packet_count() > 0);
	CHECK(server->get_status() == PacketPeerDTLS::STATUS_CONNECTED);
	REQUIRE(server->get_packet(&received, received_size) == OK);
	REQUIRE(received_size == int(sizeof(message)));
	CHECK(memcmp(received, message, sizeof(message)) == 0);

	const uint8_t response[] = { 2, 7, 1, 8 };
	REQUIRE(server->put_packet(response, sizeof(response)) == OK);
	server_transport->transfer_to(client_transport);
	REQUIRE(client->get_packet(&received, received_size) == OK);
	REQUIRE(received_size == int(sizeof(response)));
	CHECK(memcmp(received, response, sizeof(response)) == 0);
}


#ifdef B01_ALLOCATION_TESTS
void setup_failure_does_not_accept() {
	CryptoMbedTLS crypto;
	const Ref<CryptoKey> key = crypto.generate_rsa(2048);
	const Ref<X509Certificate> certificate = crypto.generate_self_signed_certificate(key, "CN=localhost,O=Godot R4 test,C=GB", "20200101000000", "20371231235959");
	REQUIRE(certificate.is_valid());
	const Ref<TLSOptions> options = TLSOptions::server(key, certificate);
	Ref<CookieContextMbedTLS> cookies;
	cookies.instantiate();
	REQUIRE(cookies->setup() == OK);
	for (int occurrence : { 1, 2 }) {
		Ref<ScriptedUDP> transport;
		transport.instantiate();
		Ref<DTLSProbe> server;
		server.instantiate();
		Error error;
		int failures;
		ERR_PRINT_OFF;
		{
			TestAllocation::Failure fault(godot_test_tls_input_buffer_size(), occurrence);
			error = server->accept_peer(transport, options, cookies);
			failures = TestAllocation::failures();
		}
		ERR_PRINT_ON;
		CHECK(failures == 1);
		CHECK(error != OK);
		CHECK(server->get_status() == PacketPeerDTLS::STATUS_DISCONNECTED);
		CHECK(transport->reads == 0);
		CHECK(transport->outgoing.is_empty());
		CHECK(server->accept_peer(transport, options, cookies) == OK);
		CHECK(server->get_status() == PacketPeerDTLS::STATUS_HANDSHAKING);
		server->disconnect_from_peer();
	}
}
#endif
} // namespace TestDTLSReceive
