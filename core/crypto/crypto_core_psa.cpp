/**************************************************************************/
/*  crypto_core_psa.cpp                                                       */
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

#include "crypto_core.h"

#include <mbedtls/version.h>

#if MBEDTLS_VERSION_MAJOR >= 4
#include "core/os/condition_variable.h"
#include "core/os/memory.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/string/ustring.h"

#include <mbedtls/base64.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/threading.h>
#include <psa/crypto.h>
#include <psa/crypto_extra.h>

namespace {
Mutex crypto_init_mutex;
bool crypto_initialized = false;

#ifdef GODOT_MBEDTLS_THREADING_ALT
int crypto_mutex_init(mbedtls_platform_mutex_t *p_mutex) {
	p_mutex->mutex = memnew(THREADING_NAMESPACE::mutex);
	return 0;
}
void crypto_mutex_free(mbedtls_platform_mutex_t *p_mutex) {
	memdelete(static_cast<THREADING_NAMESPACE::mutex *>(p_mutex->mutex));
	p_mutex->mutex = nullptr;
}
int crypto_mutex_lock(mbedtls_platform_mutex_t *p_mutex) {
	if (!p_mutex || !p_mutex->mutex) {
		return MBEDTLS_ERR_THREADING_USAGE_ERROR;
	}
	static_cast<THREADING_NAMESPACE::mutex *>(p_mutex->mutex)->lock();
	return 0;
}
int crypto_mutex_unlock(mbedtls_platform_mutex_t *p_mutex) {
	if (!p_mutex || !p_mutex->mutex) {
		return MBEDTLS_ERR_THREADING_USAGE_ERROR;
	}
	static_cast<THREADING_NAMESPACE::mutex *>(p_mutex->mutex)->unlock();
	return 0;
}
int crypto_condition_init(mbedtls_platform_condition_variable_t *p_condition) {
	p_condition->condition = memnew(THREADING_NAMESPACE::condition_variable);
	return 0;
}
void crypto_condition_free(mbedtls_platform_condition_variable_t *p_condition) {
	memdelete(static_cast<THREADING_NAMESPACE::condition_variable *>(p_condition->condition));
	p_condition->condition = nullptr;
}
int crypto_condition_signal(mbedtls_platform_condition_variable_t *p_condition) {
	static_cast<THREADING_NAMESPACE::condition_variable *>(p_condition->condition)->notify_one();
	return 0;
}
int crypto_condition_broadcast(mbedtls_platform_condition_variable_t *p_condition) {
	static_cast<THREADING_NAMESPACE::condition_variable *>(p_condition->condition)->notify_all();
	return 0;
}
int crypto_condition_wait(mbedtls_platform_condition_variable_t *p_condition, mbedtls_platform_mutex_t *p_mutex) {
	THREADING_NAMESPACE::unique_lock<THREADING_NAMESPACE::mutex> lock(*static_cast<THREADING_NAMESPACE::mutex *>(p_mutex->mutex), std::adopt_lock);
	static_cast<THREADING_NAMESPACE::condition_variable *>(p_condition->condition)->wait(lock);
	lock.release(); // The caller still owns the mutex when wait returns.
	return 0;
}
#endif

struct AESState {
	uint8_t key[32] = {};
	size_t bits = 0;
	psa_key_id_t keys[3] = {};

	void clear() {
		for (psa_key_id_t &id : keys) {
			if (id) {
				psa_destroy_key(id);
				id = 0;
			}
		}
		mbedtls_platform_zeroize(key, sizeof(key));
		bits = 0;
	}
	~AESState() { clear(); }
};

Error aes_set_key(void *p_ctx, const uint8_t *p_key, size_t p_bits) {
	ERR_FAIL_COND_V(p_bits != 128 && p_bits != 192 && p_bits != 256, ERR_INVALID_PARAMETER);
	ERR_FAIL_NULL_V(p_key, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(CryptoCore::initialize_crypto_library() != OK, FAILED);
	AESState *state = static_cast<AESState *>(p_ctx);
	state->clear();
	memcpy(state->key, p_key, p_bits / 8);
	state->bits = p_bits;
	return OK;
}

Error aes_crypt(void *p_ctx, psa_algorithm_t p_algorithm, bool p_encrypt, size_t p_length, uint8_t *r_iv, const uint8_t *p_src, uint8_t *r_dst) {
	AESState *state = static_cast<AESState *>(p_ctx);
	ERR_FAIL_COND_V(state->bits == 0, ERR_UNCONFIGURED);
	if (p_length == 0) {
		return OK;
	}
	if (p_algorithm != PSA_ALG_CFB && p_length % 16 != 0) {
		return ERR_INVALID_PARAMETER;
	}
	const int slot = p_algorithm == PSA_ALG_ECB_NO_PADDING ? 0 : (p_algorithm == PSA_ALG_CBC_NO_PADDING ? 1 : 2);
	if (!state->keys[slot]) {
		psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
		psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
		psa_set_key_bits(&attributes, state->bits);
		psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
		psa_set_key_algorithm(&attributes, p_algorithm);
		psa_status_t status = psa_import_key(&attributes, state->key, state->bits / 8, &state->keys[slot]);
		psa_reset_key_attributes(&attributes);
		ERR_FAIL_COND_V(status != PSA_SUCCESS, FAILED);
	}
	// Save feedback before in-place decryption overwrites the ciphertext.
	uint8_t feedback[16] = {};
	const size_t tail = p_algorithm == PSA_ALG_CFB ? p_length % 16 : 0;
	if (r_iv && !p_encrypt) {
		if (p_length >= 16) {
			memcpy(feedback, p_src + p_length - tail - 16, 16);
		} else {
			memcpy(feedback, r_iv, 16);
		}
	}
	uint8_t partial[16] = {};
	if (!p_encrypt && tail) {
		memcpy(partial, p_src + p_length - tail, tail);
	}
	if (r_iv && p_encrypt && p_length < 16) {
		memcpy(feedback, r_iv, 16);
	}
	psa_cipher_operation_t operation = PSA_CIPHER_OPERATION_INIT;
	psa_status_t status = p_encrypt ? psa_cipher_encrypt_setup(&operation, state->keys[slot], p_algorithm) : psa_cipher_decrypt_setup(&operation, state->keys[slot], p_algorithm);
	if (status == PSA_SUCCESS && r_iv) {
		status = psa_cipher_set_iv(&operation, r_iv, 16);
	}
	size_t written = 0;
	if (status == PSA_SUCCESS) {
		status = psa_cipher_update(&operation, p_src, p_length, r_dst, p_length, &written);
	}
	uint8_t finish[16];
	size_t finished = 0;
	if (status == PSA_SUCCESS) {
		status = psa_cipher_finish(&operation, finish, sizeof(finish), &finished);
	}
	psa_cipher_abort(&operation);
	if (status == PSA_SUCCESS && written <= p_length && finished <= p_length - written) {
		memcpy(r_dst + written, finish, finished);
	}
	ERR_FAIL_COND_V(status != PSA_SUCCESS || written + finished != p_length, FAILED);
	if (r_iv) {
		if (p_encrypt && p_length >= 16) {
			memcpy(feedback, r_dst + p_length - tail - 16, 16);
		}
		if (tail) {
			// Match mbedtls_aes_crypt_cfb128's IV state even for a partial block.
			ERR_FAIL_COND_V(aes_crypt(p_ctx, PSA_ALG_ECB_NO_PADDING, true, 16, nullptr, feedback, r_iv) != OK, FAILED);
			memcpy(r_iv, p_encrypt ? r_dst + p_length - tail : partial, tail);
		} else {
			memcpy(r_iv, feedback, 16);
		}
	}
	return OK;
}
} // namespace

extern "C" psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t *, uint8_t *r_output, size_t p_size, size_t *r_length) {
	*r_length = 0;
	if (OS::get_singleton()->get_entropy(r_output, p_size) != OK) {
		return PSA_ERROR_INSUFFICIENT_ENTROPY;
	}
	*r_length = p_size;
	return PSA_SUCCESS;
}

Error CryptoCore::initialize_crypto_library() {
	MutexLock lock(crypto_init_mutex);
	if (crypto_initialized) {
		return OK;
	}
#ifdef GODOT_MBEDTLS_THREADING_ALT
	mbedtls_threading_set_alt(crypto_mutex_init, crypto_mutex_free, crypto_mutex_lock, crypto_mutex_unlock,
			crypto_condition_init, crypto_condition_free, crypto_condition_signal, crypto_condition_broadcast, crypto_condition_wait);
#endif
	psa_status_t status = psa_crypto_init();
	if (status != PSA_SUCCESS) {
		mbedtls_psa_crypto_free();
#ifdef GODOT_MBEDTLS_THREADING_ALT
		mbedtls_threading_free_alt();
#endif
		return FAILED;
	}
	crypto_initialized = true;
	return OK;
}

void CryptoCore::finalize_crypto_library() {
	MutexLock lock(crypto_init_mutex);
	if (!crypto_initialized) {
		return;
	}
	mbedtls_psa_crypto_free();
#ifdef GODOT_MBEDTLS_THREADING_ALT
	mbedtls_threading_free_alt();
#endif
	crypto_initialized = false;
}

CryptoCore::RandomGenerator::RandomGenerator() {}
CryptoCore::RandomGenerator::~RandomGenerator() {}
Error CryptoCore::RandomGenerator::init() {
	ctx = this;
	return OK;
}
Error CryptoCore::RandomGenerator::get_random_bytes(uint8_t *r_buffer, size_t p_bytes) {
	ERR_FAIL_NULL_V(ctx, ERR_UNCONFIGURED);
	return OS::get_singleton()->get_entropy(r_buffer, p_bytes);
}

CryptoCore::AESContext::AESContext() {
	ctx = memnew(AESState);
}
CryptoCore::AESContext::~AESContext() {
	memdelete(static_cast<AESState *>(ctx));
}
Error CryptoCore::AESContext::set_encode_key(const uint8_t *p_key, size_t p_bits) {
	return aes_set_key(ctx, p_key, p_bits);
}
Error CryptoCore::AESContext::set_decode_key(const uint8_t *p_key, size_t p_bits) {
	return aes_set_key(ctx, p_key, p_bits);
}
Error CryptoCore::AESContext::encrypt_ecb(const uint8_t p_src[16], uint8_t r_dst[16]) {
	return aes_crypt(ctx, PSA_ALG_ECB_NO_PADDING, true, 16, nullptr, p_src, r_dst);
}
Error CryptoCore::AESContext::decrypt_ecb(const uint8_t p_src[16], uint8_t r_dst[16]) {
	return aes_crypt(ctx, PSA_ALG_ECB_NO_PADDING, false, 16, nullptr, p_src, r_dst);
}
Error CryptoCore::AESContext::encrypt_cbc(size_t p_length, uint8_t r_iv[16], const uint8_t *p_src, uint8_t *r_dst) {
	return aes_crypt(ctx, PSA_ALG_CBC_NO_PADDING, true, p_length, r_iv, p_src, r_dst);
}
Error CryptoCore::AESContext::decrypt_cbc(size_t p_length, uint8_t r_iv[16], const uint8_t *p_src, uint8_t *r_dst) {
	return aes_crypt(ctx, PSA_ALG_CBC_NO_PADDING, false, p_length, r_iv, p_src, r_dst);
}
Error CryptoCore::AESContext::encrypt_cfb(size_t p_length, uint8_t p_iv[16], const uint8_t *p_src, uint8_t *r_dst) {
	return aes_crypt(ctx, PSA_ALG_CFB, true, p_length, p_iv, p_src, r_dst);
}
Error CryptoCore::AESContext::decrypt_cfb(size_t p_length, uint8_t p_iv[16], const uint8_t *p_src, uint8_t *r_dst) {
	return aes_crypt(ctx, PSA_ALG_CFB, false, p_length, p_iv, p_src, r_dst);
}

String CryptoCore::b64_encode_str(const uint8_t *p_src, size_t p_src_len) {
	Vector<uint8_t> buffer;
	buffer.resize(p_src_len / 3 * 4 + 5);
	size_t length = 0;
	if (b64_encode(buffer.ptrw(), buffer.size(), &length, p_src, p_src_len) != OK) {
		return String();
	}
	buffer.write[length] = 0;
	return String::utf8(reinterpret_cast<const char *>(buffer.ptr()));
}
Error CryptoCore::b64_encode(uint8_t *r_dst, size_t p_dst_len, size_t *r_len, const uint8_t *p_src, size_t p_src_len) {
	return mbedtls_base64_encode(r_dst, p_dst_len, r_len, p_src, p_src_len) == 0 ? OK : FAILED;
}
Error CryptoCore::b64_decode(uint8_t *r_dst, size_t p_dst_len, size_t *r_len, const uint8_t *p_src, size_t p_src_len) {
	return mbedtls_base64_decode(r_dst, p_dst_len, r_len, p_src, p_src_len) == 0 ? OK : FAILED;
}

CryptoCore::MD5Context::MD5Context() {
	ctx = memnew(psa_hash_operation_t(PSA_HASH_OPERATION_INIT));
}
CryptoCore::MD5Context::~MD5Context() {
	psa_hash_abort(static_cast<psa_hash_operation_t *>(ctx));
	memdelete(static_cast<psa_hash_operation_t *>(ctx));
}
Error CryptoCore::MD5Context::start() {
	ERR_FAIL_COND_V(initialize_crypto_library() != OK, FAILED);
	psa_hash_abort(static_cast<psa_hash_operation_t *>(ctx));
	return psa_hash_setup(static_cast<psa_hash_operation_t *>(ctx), PSA_ALG_MD5) == PSA_SUCCESS ? OK : FAILED;
}
Error CryptoCore::MD5Context::update(const uint8_t *p_src, size_t p_len) {
	return psa_hash_update(static_cast<psa_hash_operation_t *>(ctx), p_src, p_len) == PSA_SUCCESS ? OK : FAILED;
}
Error CryptoCore::MD5Context::finish(unsigned char r_hash[16]) {
	size_t length = 0;
	return psa_hash_finish(static_cast<psa_hash_operation_t *>(ctx), r_hash, 16, &length) == PSA_SUCCESS && length == 16 ? OK : FAILED;
}
Error CryptoCore::md5(const uint8_t *p_src, size_t p_src_len, unsigned char r_hash[16]) {
	ERR_FAIL_COND_V(initialize_crypto_library() != OK, FAILED);
	size_t length = 0;
	return psa_hash_compute(PSA_ALG_MD5, p_src, p_src_len, r_hash, 16, &length) == PSA_SUCCESS && length == 16 ? OK : FAILED;
}

CryptoCore::SHA1Context::SHA1Context() {
	ctx = memnew(psa_hash_operation_t(PSA_HASH_OPERATION_INIT));
}
CryptoCore::SHA1Context::~SHA1Context() {
	psa_hash_abort(static_cast<psa_hash_operation_t *>(ctx));
	memdelete(static_cast<psa_hash_operation_t *>(ctx));
}
Error CryptoCore::SHA1Context::start() {
	ERR_FAIL_COND_V(initialize_crypto_library() != OK, FAILED);
	psa_hash_abort(static_cast<psa_hash_operation_t *>(ctx));
	return psa_hash_setup(static_cast<psa_hash_operation_t *>(ctx), PSA_ALG_SHA_1) == PSA_SUCCESS ? OK : FAILED;
}
Error CryptoCore::SHA1Context::update(const uint8_t *p_src, size_t p_len) {
	return psa_hash_update(static_cast<psa_hash_operation_t *>(ctx), p_src, p_len) == PSA_SUCCESS ? OK : FAILED;
}
Error CryptoCore::SHA1Context::finish(unsigned char r_hash[20]) {
	size_t length = 0;
	return psa_hash_finish(static_cast<psa_hash_operation_t *>(ctx), r_hash, 20, &length) == PSA_SUCCESS && length == 20 ? OK : FAILED;
}
Error CryptoCore::sha1(const uint8_t *p_src, size_t p_src_len, unsigned char r_hash[20]) {
	ERR_FAIL_COND_V(initialize_crypto_library() != OK, FAILED);
	size_t length = 0;
	return psa_hash_compute(PSA_ALG_SHA_1, p_src, p_src_len, r_hash, 20, &length) == PSA_SUCCESS && length == 20 ? OK : FAILED;
}

CryptoCore::SHA256Context::SHA256Context() {
	ctx = memnew(psa_hash_operation_t(PSA_HASH_OPERATION_INIT));
}
CryptoCore::SHA256Context::~SHA256Context() {
	psa_hash_abort(static_cast<psa_hash_operation_t *>(ctx));
	memdelete(static_cast<psa_hash_operation_t *>(ctx));
}
Error CryptoCore::SHA256Context::start() {
	ERR_FAIL_COND_V(initialize_crypto_library() != OK, FAILED);
	psa_hash_abort(static_cast<psa_hash_operation_t *>(ctx));
	return psa_hash_setup(static_cast<psa_hash_operation_t *>(ctx), PSA_ALG_SHA_256) == PSA_SUCCESS ? OK : FAILED;
}
Error CryptoCore::SHA256Context::update(const uint8_t *p_src, size_t p_len) {
	return psa_hash_update(static_cast<psa_hash_operation_t *>(ctx), p_src, p_len) == PSA_SUCCESS ? OK : FAILED;
}
Error CryptoCore::SHA256Context::finish(unsigned char r_hash[32]) {
	size_t length = 0;
	return psa_hash_finish(static_cast<psa_hash_operation_t *>(ctx), r_hash, 32, &length) == PSA_SUCCESS && length == 32 ? OK : FAILED;
}
Error CryptoCore::sha256(const uint8_t *p_src, size_t p_src_len, unsigned char r_hash[32]) {
	ERR_FAIL_COND_V(initialize_crypto_library() != OK, FAILED);
	size_t length = 0;
	return psa_hash_compute(PSA_ALG_SHA_256, p_src, p_src_len, r_hash, 32, &length) == PSA_SUCCESS && length == 32 ? OK : FAILED;
}

#else
Error CryptoCore::initialize_crypto_library() {
	return OK;
}
void CryptoCore::finalize_crypto_library() {}
#endif // MBEDTLS_VERSION_MAJOR >= 4
