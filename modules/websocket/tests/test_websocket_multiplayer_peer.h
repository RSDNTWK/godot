/**************************************************************************/
/*  test_websocket_multiplayer_peer.h                                     */
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

#include "../websocket_multiplayer_peer.h"

#include "tests/test_macros.h"

#ifndef WEB_ENABLED
namespace TestWebSocketMultiplayerPeer {
class QueuedPeerFixture : public WebSocketMultiplayerPeer {
public:
	void initialize() {
		connection_status = CONNECTION_CONNECTED;
		unique_id = 1;
		// Only identify this fixture as a server. No listener or socket is opened.
		tcp_server.instantiate();
	}

	void add_peer(int p_id) {
		peers_map[p_id] = Ref<WebSocketPeer>(WebSocketPeer::create());
	}

	void queue(int p_id, uint8_t p_value) {
		Packet packet;
		packet.source = p_id;
		packet.size = 2;
		packet.data = static_cast<uint8_t *>(memalloc(packet.size));
		packet.data[0] = p_value;
		packet.data[1] = static_cast<uint8_t>(p_value + 1);
		incoming_packets.push_back(packet);
	}
};

TEST_CASE("[WebSocketMultiplayerPeer] Disconnect retires queued packets without invalidating the borrowed packet or other peers") {
	for (bool force : { false, true }) {
		INFO(force);
		Ref<QueuedPeerFixture> peer;
		peer.instantiate();
		peer->initialize();
		peer->add_peer(2);
		peer->add_peer(3);
		peer->queue(2, 0x11);
		peer->queue(3, 0x22);
		peer->queue(2, 0x33);
		peer->queue(3, 0x44);
		const uint8_t *borrowed = nullptr;
		int size = 0;
		REQUIRE(peer->get_packet(&borrowed, size) == OK);
		REQUIRE(size == 2);
		REQUIRE(borrowed != nullptr);
		CHECK(peer->get_available_packet_count() == 3);
		peer->disconnect_peer(2, force);
		CHECK(borrowed[0] == 0x11);
		CHECK(borrowed[1] == 0x12);
		CHECK(peer->get_available_packet_count() == 2);
		for (int expected : { 0x22, 0x44 }) {
			CHECK(peer->get_packet_peer() == 3);
			const uint8_t *packet = nullptr;
			REQUIRE(peer->get_packet(&packet, size) == OK);
			REQUIRE(packet != nullptr);
			CHECK(size == 2);
			CHECK(packet[0] == expected);
			CHECK(packet[1] == expected + 1);
		}
		CHECK(peer->get_available_packet_count() == 0);
	}
}
} // namespace TestWebSocketMultiplayerPeer
#endif // WEB_ENABLED
