/**************************************************************************/
/*  test_crypto_core_psa.h                                                 */
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

#include "core/crypto/crypto_core.h"
#include "core/os/thread.h"
#include "tests/test_macros.h"

namespace TestCryptoCorePSA {

TEST_CASE("[CryptoCore] NIST AES modes and partial CFB feedback") {
	const PackedByteArray key = String("2b7e151628aed2a6abf7158809cf4f3c").hex_decode();
	const PackedByteArray initial_iv = String("000102030405060708090a0b0c0d0e0f").hex_decode();
	const PackedByteArray plain = String("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710").hex_decode();
	const PackedByteArray cfb = String("3b3fd92eb72dad20333449f8e83cfb4ac8a64537a0b3a93fcde3cdad9f1ce58b26751f67a3cbb140b1808cf187a4f4dfc04b05357c5d1c0eeac4c66f9ff7f2e6").hex_decode();
	const PackedByteArray cbc = String("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b273bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7").hex_decode();
	CryptoCore::AESContext context;
	REQUIRE(context.set_encode_key(key.ptr(), 128) == OK);
	PackedByteArray output;
	output.resize(plain.size());
	REQUIRE(context.encrypt_ecb(plain.ptr(), output.ptrw()) == OK);
	CHECK(String::hex_encode_buffer(output.ptr(), 16) == "3ad77bb40d7a3660a89ecaf32466ef97");
	PackedByteArray iv = initial_iv.duplicate();
	REQUIRE(context.encrypt_cbc(plain.size(), iv.ptrw(), plain.ptr(), output.ptrw()) == OK);
	CHECK(output == cbc);
	CHECK(iv == cbc.slice(cbc.size() - 16));
	const PackedByteArray key256 = String("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4").hex_decode();
	REQUIRE(context.set_encode_key(key256.ptr(), 256) == OK);
	REQUIRE(context.encrypt_ecb(plain.ptr(), output.ptrw()) == OK);
	CHECK(String::hex_encode_buffer(output.ptr(), 16) == "f3eed1bdb5d2a03c064b5a7e3db181f8");
	iv = initial_iv.duplicate();
	REQUIRE(context.encrypt_cfb(16, iv.ptrw(), plain.ptr(), output.ptrw()) == OK);
	CHECK(String::hex_encode_buffer(output.ptr(), 16) == "dc7e84bfda79164b7ecd8486985d3860");
	// Restore the AES-128 vector before its in-place decryption check.
	output = cbc.duplicate();
	iv = initial_iv.duplicate();
	REQUIRE(context.set_decode_key(key.ptr(), 128) == OK);
	REQUIRE(context.decrypt_cbc(output.size(), iv.ptrw(), output.ptr(), output.ptrw()) == OK);
	CHECK(output == plain);
	CHECK(iv == cbc.slice(cbc.size() - 16));
	for (int length : { 1, 15, 16, 17, 31, 32, 63, 64 }) {
		iv = initial_iv.duplicate();
		REQUIRE(context.set_encode_key(key.ptr(), 128) == OK);
		output = plain.slice(0, length);
		REQUIRE(context.encrypt_cfb(length, iv.ptrw(), output.ptr(), output.ptrw()) == OK);
		CHECK(output == cfb.slice(0, length));
		PackedByteArray encrypted_iv = iv.duplicate();
		iv = initial_iv.duplicate();
		// CFB decryption uses the encryption key schedule in the legacy backend.
		REQUIRE(context.decrypt_cfb(length, iv.ptrw(), output.ptr(), output.ptrw()) == OK);
		CHECK(output == plain.slice(0, length));
		CHECK(iv == encrypted_iv);
	}
}

#ifdef THREADS_ENABLED
struct ConcurrentCryptoState {
	bool passed = false;
};

static void concurrent_crypto_work(void *p_data) {
	auto *state = static_cast<ConcurrentCryptoState *>(p_data);
	const uint8_t key[32] = {};
	const uint8_t input[16] = {};
	const uint8_t expected[16] = { 0xdc, 0x95, 0xc0, 0x78, 0xa2, 0x40, 0x89, 0x89, 0xad, 0x48, 0xa2, 0x14, 0x92, 0x84, 0x20, 0x87 };
	for (int repeat = 0; repeat < 64; repeat++) {
		CryptoCore::AESContext context;
		uint8_t encrypted[16];
		uint8_t hash[32];
		if (context.set_encode_key(key, 256) != OK || context.encrypt_ecb(input, encrypted) != OK || memcmp(encrypted, expected, sizeof(expected)) != 0) {
			return;
		}
		if (CryptoCore::sha256(reinterpret_cast<const uint8_t *>("abc"), 3, hash) != OK || String::hex_encode_buffer(hash, 32) != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
			return;
		}
	}
	state->passed = true;
}

TEST_CASE("[CryptoCore] Concurrent key slots and hash operations") {
	Thread threads[4];
	ConcurrentCryptoState states[4];
	Thread::ID started[4];
	for (int i = 0; i < 4; i++) {
		started[i] = threads[i].start(concurrent_crypto_work, &states[i]);
	}
	for (int i = 0; i < 4; i++) {
		if (started[i] != Thread::UNASSIGNED_ID) {
			threads[i].wait_to_finish();
		}
		CHECK(started[i] != Thread::UNASSIGNED_ID);
		CHECK(states[i].passed);
	}
}
#endif

TEST_CASE("[CryptoCore] Hash vectors and context reuse") {
	const PackedByteArray input = String("abc").to_utf8_buffer();
	uint8_t digest[32];
	REQUIRE(CryptoCore::md5(input.ptr(), input.size(), digest) == OK);
	CHECK(String::hex_encode_buffer(digest, 16) == "900150983cd24fb0d6963f7d28e17f72");
	REQUIRE(CryptoCore::sha1(input.ptr(), input.size(), digest) == OK);
	CHECK(String::hex_encode_buffer(digest, 20) == "a9993e364706816aba3e25717850c26c9cd0d89d");
	CryptoCore::SHA256Context context;
	for (int repeat = 0; repeat < 2; repeat++) {
		REQUIRE(context.start() == OK);
		REQUIRE(context.update(input.ptr(), 1) == OK);
		REQUIRE(context.update(input.ptr() + 1, 2) == OK);
		REQUIRE(context.finish(digest) == OK);
		CHECK(String::hex_encode_buffer(digest, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	}
}

} // namespace TestCryptoCorePSA
