/**************************************************************************/
/*  test_enet_headers.cpp                                                 */
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

#include "core/io/packet_peer_udp.h"
#include "core/os/os.h"
#include "tests/test_macros.h"

#include "modules/enet/enet_connection.h"

#include <enet/protocol.h>

#include <cstring>

namespace TestENetHeaders {
bool bundled_protocol() {
#ifdef GODOT_ENET
	return true;
#else
	return false;
#endif
}

struct Probe {
	int decompress_calls = 0;
	int checksum_calls = 0;
	size_t input_limit = 0;
};
static thread_local Probe *checksum_probe = nullptr;

static size_t ENET_CALLBACK record_decompress(void *p_context, const enet_uint8 *, size_t p_input_limit, enet_uint8 *, size_t) {
	Probe *probe = static_cast<Probe *>(p_context);
	probe->decompress_calls++;
	probe->input_limit = p_input_limit;
	return 0; // Controlled decoder failure: no input read and no output written.
}
static enet_uint32 ENET_CALLBACK record_checksum(const ENetBuffer *, size_t) {
	if (checksum_probe) {
		checksum_probe->checksum_calls++;
	}
	return 0;
}
struct ProbeBinding {
	Probe *previous = checksum_probe;
	ProbeBinding(Probe &p_probe) { checksum_probe = &p_probe; }
	~ProbeBinding() { checksum_probe = previous; }
};

struct Host {
	ENetHost *value = nullptr;
	Host() {
		ENetAddress address = {};
		REQUIRE(enet_address_set_host(&address, "127.0.0.1") == 0);
		address.port = 0;
		value = enet_host_create(&address, 4, 1, 0, 0);
		REQUIRE(value != nullptr);
	}
	~Host() {
		if (value) {
			enet_host_destroy(value);
		}
	}
	int port() const {
		ENetAddress address = {};
		REQUIRE(enet_socket_get_address(value->socket, &address) == 0);
		return address.port;
	}
};
static bool receive_one(Host &p_host, enet_uint32 p_previous_count) {
	const uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 1000;
	while (p_host.value->totalReceivedPackets == p_previous_count && OS::get_singleton()->get_ticks_msec() < deadline) {
		ENetEvent event = {};
		const int result = enet_host_service(p_host.value, &event, 0);
		REQUIRE(result >= 0);
		if (event.type == ENET_EVENT_TYPE_RECEIVE && event.packet) {
			enet_packet_destroy(event.packet);
		}
		OS::get_singleton()->delay_usec(1000);
	}
	return p_host.value->totalReceivedPackets == p_previous_count + 1;
}

static void normal_after_rejections(Host &p_server) {
	enet_host_compress(p_server.value, nullptr);
	p_server.value->checksum = nullptr;
	Host client;
	ENetAddress destination = {};
	REQUIRE(enet_address_set_host(&destination, "127.0.0.1") == 0);
	destination.port = p_server.port();
	ENetPeer *client_peer = enet_host_connect(client.value, &destination, 1, 0);
	REQUIRE(client_peer != nullptr);
	bool connected[2] = {};
	const uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 2000;
	while ((!connected[0] || !connected[1]) && OS::get_singleton()->get_ticks_msec() < deadline) {
		ENetHost *hosts[] = { client.value, p_server.value };
		for (int side = 0; side < 2; side++) {
			ENetEvent event = {};
			const int result = enet_host_service(hosts[side], &event, 0);
			REQUIRE(result >= 0);
			if (event.type == ENET_EVENT_TYPE_CONNECT) {
				connected[side] = true;
			} else if (event.type == ENET_EVENT_TYPE_RECEIVE && event.packet) {
				enet_packet_destroy(event.packet);
			}
		}
		OS::get_singleton()->delay_usec(1000);
	}
	REQUIRE(connected[0]);
	REQUIRE(connected[1]);
	const uint8_t payload[] = { 7, 11, 13, 17 };
	ENetPacket *packet = enet_packet_create(payload, sizeof(payload), ENET_PACKET_FLAG_RELIABLE);
	REQUIRE(packet != nullptr);
	const int sent = enet_peer_send(client_peer, 0, packet);
	if (sent != 0) {
		enet_packet_destroy(packet);
	}
	REQUIRE(sent == 0);
	enet_host_flush(client.value);
	bool received = false;
	const uint64_t receive_deadline = OS::get_singleton()->get_ticks_msec() + 2000;
	while (!received && OS::get_singleton()->get_ticks_msec() < receive_deadline) {
		ENetHost *hosts[] = { client.value, p_server.value };
		for (int side = 0; side < 2; side++) {
			ENetEvent event = {};
			REQUIRE(enet_host_service(hosts[side], &event, 0) >= 0);
			if (event.type == ENET_EVENT_TYPE_RECEIVE && event.packet) {
				if (side == 1) {
					CHECK(event.packet->dataLength == sizeof(payload));
					if (event.packet->dataLength == sizeof(payload)) {
						CHECK(memcmp(event.packet->data, payload, sizeof(payload)) == 0);
						received = true;
					}
				}
				enet_packet_destroy(event.packet);
			}
		}
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(received);
}

void selected_header_boundaries() {
	Host host;
	Probe probe;
	ProbeBinding binding(probe);
	ENetCompressor compressor = {};
	compressor.context = &probe;
	compressor.decompress = record_decompress;
	Ref<PacketPeerUDP> sender = memnew(PacketPeerUDP);
	REQUIRE(sender->connect_to_host(IPAddress("127.0.0.1"), host.port()) == OK);
	uint8_t untouched[sizeof(host.value->packetData[1])];
	memset(untouched, 0xa7, sizeof(untouched));

	for (bool sent_time : { false, true }) {
		for (bool checksum : { false, true }) {
			for (bool compressed : { false, true }) {
				enet_host_compress(host.value, &compressor);
				host.value->checksum = checksum ? record_checksum : nullptr;
				const int header_size = (sent_time ? sizeof(ENetProtocolHeader) : 2) + (checksum ? 4 : 0);
				// A zero-byte UDP read is the transport's no-data result. Exercise
				// each reachable short length and both complete-header boundaries.
				for (int length = 1; length <= header_size + 1; length++) {
					probe = Probe();
					Vector<uint8_t> datagram;
					REQUIRE(datagram.resize(length) == OK);
					memset(datagram.ptrw(), 0, length);
					const enet_uint16 flags = ENET_PROTOCOL_MAXIMUM_PEER_ID |
							(sent_time ? ENET_PROTOCOL_HEADER_FLAG_SENT_TIME : 0) |
							(compressed ? ENET_PROTOCOL_HEADER_FLAG_COMPRESSED : 0);
					const enet_uint16 network_flags = ENET_HOST_TO_NET_16(flags);
					memcpy(datagram.ptrw(), &network_flags, MIN(length, 2));
					memset(host.value->packetData[0], 0xa7, sizeof(host.value->packetData[0]));
					memset(host.value->packetData[1], 0xa7, sizeof(host.value->packetData[1]));
					const enet_uint32 previous_count = host.value->totalReceivedPackets;
					REQUIRE(sender->put_packet(datagram.ptr(), datagram.size()) == OK);
					REQUIRE(receive_one(host, previous_count));
					if (length < header_size) {
						CHECK(probe.decompress_calls == 0);
						CHECK(probe.checksum_calls == 0);
						for (int i = length; i < header_size; i++) {
							CHECK(host.value->packetData[0][i] == 0xa7);
						}
						CHECK(memcmp(host.value->packetData[1], untouched, sizeof(untouched)) == 0);
					} else {
						CHECK(probe.decompress_calls == (compressed ? 1 : 0));
						CHECK(probe.checksum_calls == (checksum && !compressed ? 1 : 0));
						if (compressed) {
							CHECK(probe.input_limit == size_t(length - header_size));
						}
					}
				}
			}
		}
	}
	normal_after_rejections(host);
}

void ordinary_compression_connections() {
	for (int mode = ENetConnection::COMPRESS_NONE; mode <= ENetConnection::COMPRESS_ZSTD; mode++) {
		INFO("compression mode ", mode);
		Ref<ENetConnection> server = memnew(ENetConnection);
		Ref<ENetConnection> client = memnew(ENetConnection);
		REQUIRE(server->create_host_bound(IPAddress("127.0.0.1"), 0, 1, 1) == OK);
		REQUIRE(client->create_host_bound(IPAddress("127.0.0.1"), 0, 1, 1) == OK);
		server->compress(ENetConnection::CompressionMode(mode));
		client->compress(ENetConnection::CompressionMode(mode));
		Ref<ENetPacketPeer> client_peer = client->connect_to_host("127.0.0.1", server->get_local_port(), 1);
		REQUIRE(client_peer.is_valid());
		Ref<ENetPacketPeer> server_peer;
		bool connected[2] = {};
		Ref<ENetConnection> connections[] = { client, server };
		const uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 2000;
		while ((!connected[0] || !connected[1]) && OS::get_singleton()->get_ticks_msec() < deadline) {
			for (int side = 0; side < 2; side++) {
				ENetConnection::Event event;
				const ENetConnection::EventType type = connections[side]->service(0, event);
				REQUIRE(type != ENetConnection::EVENT_ERROR);
				if (type == ENetConnection::EVENT_CONNECT) {
					connected[side] = true;
					if (side == 1) {
						server_peer = event.peer;
					}
				} else if (type == ENetConnection::EVENT_RECEIVE && event.packet) {
					enet_packet_destroy(event.packet);
				}
			}
			OS::get_singleton()->delay_usec(1000);
		}
		REQUIRE(connected[0]);
		REQUIRE(connected[1]);
		REQUIRE(server_peer.is_valid());
		Vector<uint8_t> payload;
		REQUIRE(payload.resize(512) == OK);
		for (int i = 0; i < payload.size(); i++) {
			payload.write[i] = uint8_t(i % 13);
		}
		REQUIRE(client_peer->put_packet(payload.ptr(), payload.size()) == OK);
		REQUIRE(server_peer->put_packet(payload.ptr(), payload.size()) == OK);
		client->flush();
		server->flush();
		bool received[2] = {};
		const uint64_t receive_deadline = OS::get_singleton()->get_ticks_msec() + 2000;
		while ((!received[0] || !received[1]) && OS::get_singleton()->get_ticks_msec() < receive_deadline) {
			for (int side = 0; side < 2; side++) {
				ENetConnection::Event event;
				const ENetConnection::EventType type = connections[side]->service(0, event);
				REQUIRE(type != ENetConnection::EVENT_ERROR);
				if (type == ENetConnection::EVENT_RECEIVE && event.packet) {
					CHECK(event.packet->dataLength == size_t(payload.size()));
					if (event.packet->dataLength == size_t(payload.size())) {
						CHECK(memcmp(event.packet->data, payload.ptr(), payload.size()) == 0);
						received[side] = true;
					}
					enet_packet_destroy(event.packet);
				}
			}
			OS::get_singleton()->delay_usec(1000);
		}
		CHECK(received[0]);
		CHECK(received[1]);
		CHECK(client_peer->get_state() == ENetPacketPeer::STATE_CONNECTED);
		CHECK(server_peer->get_state() == ENetPacketPeer::STATE_CONNECTED);
	}
}
} // namespace TestENetHeaders
