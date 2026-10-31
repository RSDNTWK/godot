/**************************************************************************/
/*  test_tls_initialization.cpp                                                 */
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
#include "../stream_peer_mbedtls.h"
#include "core/io/stream_peer.h"
#endif
#include "test_tls_initialization.h"
#include "../crypto_mbedtls.h"
#include "../tls_context_mbedtls.h"
#include "tests/core/io/allocation_test_helpers.h"
namespace TestTLSInitialization {
class Context : public TLSContextMbedTLS {
public:
	bool active() const { return inited; }
};
struct Quiet {
	bool previous = CoreGlobals::print_error_enabled;
	Quiet() { CoreGlobals::print_error_enabled = false; }
	~Quiet() { CoreGlobals::print_error_enabled = previous; }
};
static void check_cleared(const Context &p_context) {
	CHECK_FALSE(p_context.active());
	CHECK(p_context.tls.MBEDTLS_PRIVATE(conf) == nullptr);
	CHECK(p_context.certs.is_null());
	CHECK(p_context.pkey.is_null());
	CHECK(p_context.cookies.is_null());
}
void hostname_and_client_controls() {
	Ref<X509CertificateMbedTLS> chain;
	chain.instantiate();
	const Ref<TLSOptions> options = TLSOptions::client(chain);
	Context context;
	REQUIRE(context.init_client(MBEDTLS_SSL_TRANSPORT_STREAM, "localhost", options) == OK);
	CHECK(context.conf.MBEDTLS_PRIVATE(authmode) == MBEDTLS_SSL_VERIFY_REQUIRED);
	context.clear();
	{
		Quiet quiet;
		CHECK(context.init_client(MBEDTLS_SSL_TRANSPORT_STREAM, String("a").repeat(256), options) != OK);
	}
	check_cleared(context);
	CHECK(context.init_client(MBEDTLS_SSL_TRANSPORT_STREAM, "localhost", options) == OK);
	context.clear();
	const Ref<TLSOptions> override_options = TLSOptions::client(chain, "localhost");
	CHECK(context.init_client(MBEDTLS_SSL_TRANSPORT_STREAM, String("a").repeat(256), override_options) == OK);
	context.clear();
	CHECK(context.init_client(MBEDTLS_SSL_TRANSPORT_STREAM, "localhost", TLSOptions::client_unsafe(chain)) == OK);
	CHECK(context.conf.MBEDTLS_PRIVATE(authmode) == MBEDTLS_SSL_VERIFY_REQUIRED);
	context.clear();
}
#ifdef B01_ALLOCATION_TESTS
void allocation_failures() {
	CryptoMbedTLS crypto;
	const Ref<CryptoKey> key = crypto.generate_rsa(2048);
	REQUIRE(key.is_valid());
	const Ref<X509Certificate> certificate = crypto.generate_self_signed_certificate(key, "CN=localhost,O=Godot R4 test,C=GB", "20200101000000", "20371231235959");
	REQUIRE(certificate.is_valid());
	const Ref<TLSOptions> client = TLSOptions::client(certificate);
	const Ref<TLSOptions> server = TLSOptions::server(key, certificate);
	const String hostname = "localhost";
	Context context;
	REQUIRE(context.init_client(MBEDTLS_SSL_TRANSPORT_STREAM, hostname, client) == OK);
	context.clear();
	REQUIRE(context.init_server(MBEDTLS_SSL_TRANSPORT_STREAM, server) == OK);
	context.clear();
	struct Fault { size_t size; int occurrence; bool server; };
	const Fault faults[] = {
		{ 10, 1, false },
		{ godot_test_tls_input_buffer_size(), 1, false },
		{ godot_test_tls_output_buffer_size(), godot_test_tls_input_buffer_size() == godot_test_tls_output_buffer_size() ? 2 : 1, false },
		{ godot_test_tls_input_buffer_size(), 1, true },
		{ godot_test_tls_output_buffer_size(), godot_test_tls_input_buffer_size() == godot_test_tls_output_buffer_size() ? 2 : 1, true },
	};
	for (const Fault &fault : faults) {
		Error error;
		int failures;
		{
			Quiet quiet;
			TestAllocation::Failure injected(fault.size, fault.occurrence);
			error = fault.server ? context.init_server(MBEDTLS_SSL_TRANSPORT_STREAM, server) : context.init_client(MBEDTLS_SSL_TRANSPORT_STREAM, hostname, client);
			failures = TestAllocation::failures();
		}
		CHECK(failures == 1);
		CHECK(error != OK);
		check_cleared(context);
		CHECK((fault.server ? context.init_server(MBEDTLS_SSL_TRANSPORT_STREAM, server) : context.init_client(MBEDTLS_SSL_TRANSPORT_STREAM, hostname, client)) == OK);
		context.clear();

		// Public wrappers must return before binding a transport or handshaking.
		Ref<StreamPeerBuffer> transport;
		transport.instantiate();
		Ref<StreamPeerMbedTLS> peer;
		peer.instantiate();
		{
			Quiet quiet;
			TestAllocation::Failure injected(fault.size, fault.occurrence);
			error = fault.server ? peer->accept_stream(transport, server) : peer->connect_to_stream(transport, hostname, client);
			failures = TestAllocation::failures();
		}
		CHECK(failures == 1);
		CHECK(error != OK);
		CHECK(peer->get_status() == StreamPeerTLS::STATUS_DISCONNECTED);
		CHECK(peer->get_stream().is_null());
		CHECK(transport->get_data_array().is_empty());
	}
}
#endif
}
