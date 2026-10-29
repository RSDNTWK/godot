/**************************************************************************/
/*  test_enet_lifecycle.cpp                                               */
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

#include "core/os/os.h"
#include "tests/test_macros.h"

#include "modules/enet/enet_connection.h"

#include <cstring>

namespace TestENetLifecycle {
static Ref<ENetConnection> host(int p_capacity) {
	Ref<ENetConnection> result = memnew(ENetConnection);
	REQUIRE(result->create_host_bound(IPAddress("127.0.0.1"), 0, p_capacity, 1) == OK);
	return result;
}
void pending_peer_cleanup() {
	for (bool immediate : { false, true }) {
		Ref<ENetConnection> receiver = host(1);
		Ref<ENetConnection> client = host(1);
		for (int repeat = 0; repeat < 4; repeat++) {
			// Initiation retains a peer before handshake completion. Resetting
			// first clears its queued connect work before the service call.
			Ref<ENetPacketPeer> peer = client->connect_to_host("127.0.0.1", receiver->get_local_port(), 1);
			REQUIRE(peer.is_valid());
			REQUIRE(peer->is_active());
			if (immediate) {
				peer->peer_disconnect_now();
			} else {
				peer->reset();
			}
			CHECK_FALSE(peer->is_active());
			if (repeat % 2) {
				peer.unref(); // Peer lifetime and list-node lifetime differ.
			}
			for (int poll = 0; poll < 3; poll++) {
				ENetConnection::Event event;
				CHECK(client->service(0, event) == ENetConnection::EVENT_NONE);
			}
			List<Ref<ENetPacketPeer>> peers;
			client->get_peers(peers);
			CHECK(peers.is_empty());
			// The next iteration's connect also proves the private list is empty.
		}
	}
}

void mixed_peer_cleanup() {
	const int patterns[][2] = { { 0, 3 }, { 0, 1 }, { 1, 2 }, { 2, 3 } };
	for (const auto &inactive : patterns) {
		Ref<ENetConnection> server = host(4);
		Ref<ENetConnection> clients[4];
		Ref<ENetPacketPeer> client_peers[4];
		bool connected[4] = {};
		Vector<Ref<ENetPacketPeer>> server_peers;
		for (int i = 0; i < 4; i++) {
			clients[i] = host(1);
			client_peers[i] = clients[i]->connect_to_host("127.0.0.1", server->get_local_port(), 1);
			REQUIRE(client_peers[i].is_valid());
		}
		const uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 3000;
		bool all_connected = false;
		while (!all_connected && OS::get_singleton()->get_ticks_msec() < deadline) {
			for (int i = 0; i < 4; i++) {
				ENetConnection::Event event;
				const ENetConnection::EventType type = clients[i]->service(0, event);
				REQUIRE(type != ENetConnection::EVENT_ERROR);
				if (type == ENetConnection::EVENT_CONNECT) {
					connected[i] = true;
				} else if (type == ENetConnection::EVENT_RECEIVE && event.packet) {
					enet_packet_destroy(event.packet);
				}
			}
			for (int poll = 0; poll < 8; poll++) {
				ENetConnection::Event event;
				const ENetConnection::EventType type = server->service(0, event);
				REQUIRE(type != ENetConnection::EVENT_ERROR);
				if (type == ENetConnection::EVENT_CONNECT) {
					server_peers.push_back(event.peer);
				} else if (type == ENetConnection::EVENT_RECEIVE && event.packet) {
					enet_packet_destroy(event.packet);
				} else if (type == ENetConnection::EVENT_NONE) {
					break;
				}
			}
			all_connected = server_peers.size() == 4;
			for (bool state : connected) {
				all_connected = all_connected && state;
			}
			OS::get_singleton()->delay_usec(1000);
		}
		REQUIRE(all_connected);
		server_peers[inactive[0]]->reset();
		server_peers[inactive[1]]->peer_disconnect_now();
		CHECK_FALSE(server_peers[inactive[0]]->is_active());
		CHECK_FALSE(server_peers[inactive[1]]->is_active());
		server_peers.write[inactive[1]].unref();

		ENetConnection::Event cleanup_event;
		CHECK(server->service(0, cleanup_event) != ENetConnection::EVENT_ERROR);
		List<Ref<ENetPacketPeer>> remaining;
		server->get_peers(remaining);
		REQUIRE(remaining.size() == 2);
		int expected = 0;
		for (const Ref<ENetPacketPeer> &peer : remaining) {
			while (expected == inactive[0] || expected == inactive[1]) {
				expected++;
			}
			REQUIRE(expected < 4);
			CHECK(peer == server_peers[expected]);
			CHECK(peer->get_state() == ENetPacketPeer::STATE_CONNECTED);
			expected++;
		}
		Ref<ENetPacketPeer> survivor = remaining.front()->get();
		int target = -1;
		for (int i = 0; i < 4; i++) {
			if (clients[i]->get_local_port() == survivor->get_remote_port()) {
				target = i;
			}
		}
		REQUIRE(target >= 0);
		const uint8_t payload[] = { 2, 3, 5, 7, 11 };
		REQUIRE(survivor->put_packet(payload, sizeof(payload)) == OK);
		server->flush();
		bool received = false;
		const uint64_t receive_deadline = OS::get_singleton()->get_ticks_msec() + 2000;
		while (!received && OS::get_singleton()->get_ticks_msec() < receive_deadline) {
			ENetConnection::Event event;
			const ENetConnection::EventType type = clients[target]->service(0, event);
			REQUIRE(type != ENetConnection::EVENT_ERROR);
			if (type == ENetConnection::EVENT_RECEIVE && event.packet) {
				CHECK(event.packet->dataLength == sizeof(payload));
				if (event.packet->dataLength == sizeof(payload)) {
					CHECK(memcmp(event.packet->data, payload, sizeof(payload)) == 0);
					received = true;
				}
				enet_packet_destroy(event.packet);
			}
			ENetConnection::Event server_event;
			CHECK(server->service(0, server_event) != ENetConnection::EVENT_ERROR);
			OS::get_singleton()->delay_usec(1000);
		}
		CHECK(received);
		for (const Ref<ENetPacketPeer> &peer : remaining) {
			peer->reset();
		}
		ENetConnection::Event final_cleanup;
		CHECK(server->service(0, final_cleanup) != ENetConnection::EVENT_ERROR);
		List<Ref<ENetPacketPeer>> empty;
		server->get_peers(empty);
		CHECK(empty.is_empty());
		Ref<ENetConnection> receiver = host(1);
		Ref<ENetPacketPeer> reconnect = server->connect_to_host("127.0.0.1", receiver->get_local_port(), 1);
		REQUIRE(reconnect.is_valid()); // connect_to_host requires the internal list to be empty.
		reconnect->reset();
		ENetConnection::Event last_cleanup;
		CHECK(server->service(0, last_cleanup) != ENetConnection::EVENT_ERROR);
	}
}
} // namespace TestENetLifecycle
