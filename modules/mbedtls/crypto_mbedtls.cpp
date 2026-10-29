/**************************************************************************/
/*  crypto_mbedtls.cpp                                                    */
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

#include "crypto_mbedtls.h"

#include "core/crypto/crypto_core.h"
#include "core/io/certs_compressed.gen.h"
#include "core/io/compression.h"
#include "core/io/file_access.h"
#include "core/object/class_db.h"
#include "core/os/os.h"

#include <mbedtls/debug.h>
#include <mbedtls/md.h>
#include <mbedtls/pem.h>

#if MBEDTLS_VERSION_MAJOR >= 4
struct GodotHMACPSA {
	psa_mac_operation_t operation = PSA_MAC_OPERATION_INIT;
	psa_key_id_t key = 0;
	~GodotHMACPSA() {
		psa_mac_abort(&operation);
		if (key) {
			psa_destroy_key(key);
		}
	}
};
#endif

#define PEM_BEGIN_CRT "-----BEGIN CERTIFICATE-----\n"
#define PEM_END_CRT "-----END CERTIFICATE-----\n"
#define PEM_MIN_SIZE 54

CryptoKey *CryptoKeyMbedTLS::create(bool p_notify_postinitialize) {
	return static_cast<CryptoKey *>(ClassDB::creator<CryptoKeyMbedTLS>(p_notify_postinitialize));
}

Error CryptoKeyMbedTLS::load(const String &p_path, bool p_public_only) {
	ERR_FAIL_COND_V_MSG(locks, ERR_ALREADY_IN_USE, "Key is in use");

	PackedByteArray out;
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	ERR_FAIL_COND_V_MSG(f.is_null(), ERR_INVALID_PARAMETER, "Cannot open CryptoKeyMbedTLS file '" + p_path + "'.");

	uint64_t flen = f->get_length();
	out.resize(flen + 1);
	f->get_buffer(out.ptrw(), flen);
	out.write[flen] = 0; // string terminator

	int ret = 0;
	if (p_public_only) {
		ret = mbedtls_pk_parse_public_key(&pkey, out.ptr(), out.size());
	} else {
		ret = _parse_key(out.ptr(), out.size());
	}
	// We MUST zeroize the memory for safety!
	mbedtls_platform_zeroize(out.ptrw(), out.size());
	ERR_FAIL_COND_V_MSG(ret, FAILED, "Error parsing key '" + itos(ret) + "'.");

	public_only = p_public_only;
	return OK;
}

Error CryptoKeyMbedTLS::save(const String &p_path, bool p_public_only) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	ERR_FAIL_COND_V_MSG(f.is_null(), ERR_INVALID_PARAMETER, "Cannot save CryptoKeyMbedTLS file '" + p_path + "'.");

	unsigned char w[16000];
	memset(w, 0, sizeof(w));

	int ret = 0;
	if (p_public_only) {
		ret = mbedtls_pk_write_pubkey_pem(&pkey, w, sizeof(w));
	} else {
		ret = mbedtls_pk_write_key_pem(&pkey, w, sizeof(w));
	}
	if (ret != 0) {
		mbedtls_platform_zeroize(w, sizeof(w)); // Zeroize anything we might have written.
		ERR_FAIL_V_MSG(FAILED, "Error writing key '" + itos(ret) + "'.");
	}

	size_t len = strlen((char *)w);
	f->store_buffer(w, len);
	mbedtls_platform_zeroize(w, sizeof(w)); // Zeroize temporary buffer.
	return OK;
}

Error CryptoKeyMbedTLS::load_from_string(const String &p_string_key, bool p_public_only) {
	int ret = 0;
	const CharString string_key_utf8 = p_string_key.utf8();
	if (p_public_only) {
		ret = mbedtls_pk_parse_public_key(&pkey, (const unsigned char *)string_key_utf8.get_data(), string_key_utf8.size());
	} else {
		ret = _parse_key((const unsigned char *)string_key_utf8.get_data(), string_key_utf8.size());
	}
	ERR_FAIL_COND_V_MSG(ret, FAILED, "Error parsing key '" + itos(ret) + "'.");

	public_only = p_public_only;
	return OK;
}

String CryptoKeyMbedTLS::save_to_string(bool p_public_only) {
	unsigned char w[16000];
	memset(w, 0, sizeof(w));

	int ret = 0;
	if (p_public_only) {
		ret = mbedtls_pk_write_pubkey_pem(&pkey, w, sizeof(w));
	} else {
		ret = mbedtls_pk_write_key_pem(&pkey, w, sizeof(w));
	}
	if (ret != 0) {
		mbedtls_platform_zeroize(w, sizeof(w));
		ERR_FAIL_V_MSG("", "Error saving key '" + itos(ret) + "'.");
	}
	String s = String::utf8((char *)w);
	return s;
}

int CryptoKeyMbedTLS::_parse_key(const uint8_t *p_buf, int p_size) {
#if MBEDTLS_VERSION_MAJOR >= 4
	return mbedtls_pk_parse_key(&pkey, p_buf, p_size, nullptr, 0);
#else
#if MBEDTLS_VERSION_MAJOR >= 3
	mbedtls_entropy_context rng_entropy;
	mbedtls_ctr_drbg_context rng_drbg;

	mbedtls_ctr_drbg_init(&rng_drbg);
	mbedtls_entropy_init(&rng_entropy);
	int ret = mbedtls_ctr_drbg_seed(&rng_drbg, mbedtls_entropy_func, &rng_entropy, nullptr, 0);
	ERR_FAIL_COND_V_MSG(ret != 0, ret, vformat("mbedtls_ctr_drbg_seed returned -0x%x\n", (unsigned int)-ret));

	ret = mbedtls_pk_parse_key(&pkey, p_buf, p_size, nullptr, 0, mbedtls_ctr_drbg_random, &rng_drbg);
	mbedtls_ctr_drbg_free(&rng_drbg);
	mbedtls_entropy_free(&rng_entropy);
	return ret;
#else
	return mbedtls_pk_parse_key(&pkey, p_buf, p_size, nullptr, 0);
#endif
#endif
}

X509Certificate *X509CertificateMbedTLS::create(bool p_notify_postinitialize) {
	return static_cast<X509Certificate *>(ClassDB::creator<X509CertificateMbedTLS>(p_notify_postinitialize));
}

Error X509CertificateMbedTLS::load(const String &p_path) {
	ERR_FAIL_COND_V_MSG(locks, ERR_ALREADY_IN_USE, "Certificate is already in use.");

	PackedByteArray out;
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	ERR_FAIL_COND_V_MSG(f.is_null(), ERR_INVALID_PARAMETER, vformat("Cannot open X509CertificateMbedTLS file '%s'.", p_path));

	uint64_t flen = f->get_length();
	out.resize(flen + 1);
	f->get_buffer(out.ptrw(), flen);
	out.write[flen] = 0; // string terminator

	int ret = mbedtls_x509_crt_parse(&cert, out.ptr(), out.size());
	ERR_FAIL_COND_V_MSG(ret < 0, FAILED, vformat("Error parsing X509 certificates from file '%s': %d.", p_path, ret));
	if (ret > 0) { // Some certs parsed fine, don't error.
		print_verbose(vformat("MbedTLS: Some X509 certificates could not be parsed from file '%s' (%d certificates skipped).", p_path, ret));
	}

	return OK;
}

Error X509CertificateMbedTLS::load_from_memory(const uint8_t *p_buffer, int p_len) {
	ERR_FAIL_COND_V_MSG(locks, ERR_ALREADY_IN_USE, "Certificate is already in use.");

	int ret = mbedtls_x509_crt_parse(&cert, p_buffer, p_len);
	ERR_FAIL_COND_V_MSG(ret < 0, FAILED, vformat("Error parsing X509 certificates: %d.", ret));
	if (ret > 0) { // Some certs parsed fine, don't error.
		print_verbose(vformat("MbedTLS: Some X509 certificates could not be parsed (%d certificates skipped).", ret));
	}
	return OK;
}

static Error _encode_certificate_pem(const mbedtls_x509_crt *p_certificate, Vector<uint8_t> &r_buffer) {
	size_t required = 0;
	const int size_result = mbedtls_pem_write_buffer(PEM_BEGIN_CRT, PEM_END_CRT, p_certificate->raw.p, p_certificate->raw.len, nullptr, 0, &required);
	ERR_FAIL_COND_V_MSG(size_result != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || required == 0 || required > INT32_MAX, FAILED, "Cannot determine certificate PEM size.");
	const Error allocation_error = r_buffer.resize(required);
	ERR_FAIL_COND_V(allocation_error != OK, allocation_error);
	size_t written = 0;
	const int result = mbedtls_pem_write_buffer(PEM_BEGIN_CRT, PEM_END_CRT, p_certificate->raw.p, p_certificate->raw.len, r_buffer.ptrw(), r_buffer.size(), &written);
	ERR_FAIL_COND_V_MSG(result != 0 || written != required, FAILED, "Error encoding certificate PEM.");
	return OK;
}

Error X509CertificateMbedTLS::save(const String &p_path) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	ERR_FAIL_COND_V_MSG(f.is_null(), ERR_INVALID_PARAMETER, vformat("Cannot save X509CertificateMbedTLS file '%s'.", p_path));
	for (const mbedtls_x509_crt *crt = &cert; crt; crt = crt->next) {
		Vector<uint8_t> encoded;
		const Error err = _encode_certificate_pem(crt, encoded);
		ERR_FAIL_COND_V(err != OK, err);
		ERR_FAIL_COND_V(!f->store_buffer(encoded.ptr(), encoded.size() - 1), ERR_FILE_CANT_WRITE);
	}
	return OK;
}

String X509CertificateMbedTLS::save_to_string() {
	String buffer;
	for (const mbedtls_x509_crt *crt = &cert; crt; crt = crt->next) {
		Vector<uint8_t> encoded;
		const Error err = _encode_certificate_pem(crt, encoded);
		ERR_FAIL_COND_V(err != OK, String());
		// The PEM size includes its string terminator.
		buffer += String::ascii(Span((const char *)encoded.ptr(), encoded.size() - 1));
	}
	if (buffer.length() <= PEM_MIN_SIZE) {
		return String();
	}
	return buffer;
}

Error X509CertificateMbedTLS::load_from_string(const String &p_string_key) {
	ERR_FAIL_COND_V_MSG(locks, ERR_ALREADY_IN_USE, "Certificate is already in use.");
	CharString cs = p_string_key.utf8();

	int ret = mbedtls_x509_crt_parse(&cert, (const unsigned char *)cs.get_data(), cs.size());
	ERR_FAIL_COND_V_MSG(ret < 0, FAILED, vformat("Error parsing X509 certificates: %d.", ret));
	if (ret > 0) { // Some certs parsed fine, don't error.
		print_verbose(vformat("MbedTLS: Some X509 certificates could not be parsed (%d certificates skipped).", ret));
	}

	return OK;
}

bool HMACContextMbedTLS::is_md_type_allowed(mbedtls_md_type_t p_md_type) {
	switch (p_md_type) {
		case MBEDTLS_MD_SHA1:
		case MBEDTLS_MD_SHA256:
			return true;
		default:
			return false;
	}
}

HMACContext *HMACContextMbedTLS::create(bool p_notify_postinitialize) {
	return static_cast<HMACContext *>(ClassDB::creator<HMACContextMbedTLS>(p_notify_postinitialize));
}

Error HMACContextMbedTLS::start(HashingContext::HashType p_hash_type, const PackedByteArray &p_key) {
#if MBEDTLS_VERSION_MAJOR >= 4
	ERR_FAIL_COND_V_MSG(ctx != nullptr, ERR_FILE_ALREADY_IN_USE, "HMACContext already started.");
	ERR_FAIL_COND_V_MSG(p_key.is_empty(), ERR_INVALID_PARAMETER, "Key must not be empty.");
	ERR_FAIL_COND_V(p_hash_type != HashingContext::HASH_SHA1 && p_hash_type != HashingContext::HASH_SHA256, ERR_INVALID_PARAMETER);
	hash_type = p_hash_type;
	hash_len = p_hash_type == HashingContext::HASH_SHA1 ? 20 : 32;
	psa_algorithm_t algorithm = PSA_ALG_HMAC(p_hash_type == HashingContext::HASH_SHA1 ? PSA_ALG_SHA_1 : PSA_ALG_SHA_256);
	GodotHMACPSA *state = memnew(GodotHMACPSA);
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
	psa_set_key_algorithm(&attributes, algorithm);
	// HMAC hashes keys longer than its block size. Normalize before import so
	// Godot keeps accepting arbitrary key lengths beyond PSA key-object limits.
	uint8_t normalized_key[32] = {};
	const uint8_t *key_data = p_key.ptr();
	size_t key_size = p_key.size();
	psa_status_t status = PSA_SUCCESS;
	if (key_size > 64) {
		size_t digest_size = 0;
		status = psa_hash_compute(p_hash_type == HashingContext::HASH_SHA1 ? PSA_ALG_SHA_1 : PSA_ALG_SHA_256,
				key_data, key_size, normalized_key, sizeof(normalized_key), &digest_size);
		if (status != PSA_SUCCESS || digest_size != (size_t)hash_len) {
			mbedtls_platform_zeroize(normalized_key, sizeof(normalized_key));
			psa_reset_key_attributes(&attributes);
			memdelete(state);
			hash_len = 0;
			return FAILED;
		}
		key_data = normalized_key;
		key_size = digest_size;
	}
	status = psa_import_key(&attributes, key_data, key_size, &state->key);
	mbedtls_platform_zeroize(normalized_key, sizeof(normalized_key));
	psa_reset_key_attributes(&attributes);
	if (status == PSA_SUCCESS) {
		status = psa_mac_sign_setup(&state->operation, state->key, algorithm);
	}
	if (status != PSA_SUCCESS) {
		memdelete(state);
		hash_len = 0;
		return FAILED;
	}
	ctx = state;
	return OK;
#else
	ERR_FAIL_COND_V_MSG(ctx != nullptr, ERR_FILE_ALREADY_IN_USE, "HMACContext already started.");

	// HMAC keys can be any size.
	ERR_FAIL_COND_V_MSG(p_key.is_empty(), ERR_INVALID_PARAMETER, "Key must not be empty.");

	hash_type = p_hash_type;
	mbedtls_md_type_t ht = CryptoMbedTLS::md_type_from_hashtype(p_hash_type, hash_len);

	bool allowed = HMACContextMbedTLS::is_md_type_allowed(ht);
	ERR_FAIL_COND_V_MSG(!allowed, ERR_INVALID_PARAMETER, "Unsupported hash type.");

	ctx = memalloc(sizeof(mbedtls_md_context_t));
	mbedtls_md_init((mbedtls_md_context_t *)ctx);

	mbedtls_md_setup((mbedtls_md_context_t *)ctx, mbedtls_md_info_from_type((mbedtls_md_type_t)ht), 1);
	int ret = mbedtls_md_hmac_starts((mbedtls_md_context_t *)ctx, (const uint8_t *)p_key.ptr(), (size_t)p_key.size());
	return ret ? FAILED : OK;
#endif
}

Error HMACContextMbedTLS::update(const PackedByteArray &p_data) {
#if MBEDTLS_VERSION_MAJOR >= 4
	ERR_FAIL_NULL_V_MSG(ctx, ERR_INVALID_DATA, "Start must be called before update.");
	ERR_FAIL_COND_V(p_data.is_empty(), ERR_INVALID_PARAMETER);
	return psa_mac_update(&static_cast<GodotHMACPSA *>(ctx)->operation, p_data.ptr(), p_data.size()) == PSA_SUCCESS ? OK : FAILED;
#else
	ERR_FAIL_NULL_V_MSG(ctx, ERR_INVALID_DATA, "Start must be called before update.");

	ERR_FAIL_COND_V_MSG(p_data.is_empty(), ERR_INVALID_PARAMETER, "Src must not be empty.");

	int ret = mbedtls_md_hmac_update((mbedtls_md_context_t *)ctx, (const uint8_t *)p_data.ptr(), (size_t)p_data.size());
	return ret ? FAILED : OK;
#endif
}

PackedByteArray HMACContextMbedTLS::finish() {
#if MBEDTLS_VERSION_MAJOR >= 4
	ERR_FAIL_NULL_V_MSG(ctx, PackedByteArray(), "Start must be called before finish.");
	PackedByteArray out;
	out.resize(hash_len);
	size_t size = 0;
	psa_status_t status = psa_mac_sign_finish(&static_cast<GodotHMACPSA *>(ctx)->operation, out.ptrw(), out.size(), &size);
	memdelete(static_cast<GodotHMACPSA *>(ctx));
	ctx = nullptr;
	bool valid = status == PSA_SUCCESS && size == (size_t)hash_len;
	hash_len = 0;
	ERR_FAIL_COND_V(!valid, PackedByteArray());
	return out;
#else
	ERR_FAIL_NULL_V_MSG(ctx, PackedByteArray(), "Start must be called before finish.");
	ERR_FAIL_COND_V_MSG(hash_len == 0, PackedByteArray(), "Unsupported hash type.");

	PackedByteArray out;
	out.resize(hash_len);

	unsigned char *out_ptr = (unsigned char *)out.ptrw();
	int ret = mbedtls_md_hmac_finish((mbedtls_md_context_t *)ctx, out_ptr);

	mbedtls_md_free((mbedtls_md_context_t *)ctx);
	memfree((mbedtls_md_context_t *)ctx);
	ctx = nullptr;
	hash_len = 0;

	ERR_FAIL_COND_V_MSG(ret, PackedByteArray(), "Error received while finishing HMAC");
	return out;
#endif
}

HMACContextMbedTLS::~HMACContextMbedTLS() {
#if MBEDTLS_VERSION_MAJOR >= 4
	if (ctx) {
		memdelete(static_cast<GodotHMACPSA *>(ctx));
	}
#else
	if (ctx != nullptr) {
		mbedtls_md_free((mbedtls_md_context_t *)ctx);
		memfree((mbedtls_md_context_t *)ctx);
	}
#endif
}

Crypto *CryptoMbedTLS::create(bool p_notify_postinitialize) {
	return static_cast<Crypto *>(ClassDB::creator<CryptoMbedTLS>(p_notify_postinitialize));
}

void CryptoMbedTLS::initialize_crypto() {
	Crypto::_create = create;
	Crypto::_load_default_certificates = load_default_certificates;
	X509CertificateMbedTLS::make_default();
	CryptoKeyMbedTLS::make_default();
	HMACContextMbedTLS::make_default();
}

void CryptoMbedTLS::finalize_crypto() {
	Crypto::_create = nullptr;
	Crypto::_load_default_certificates = nullptr;
	default_certs = nullptr;
	X509CertificateMbedTLS::finalize();
	CryptoKeyMbedTLS::finalize();
	HMACContextMbedTLS::finalize();
}

CryptoMbedTLS::CryptoMbedTLS() {
#if MBEDTLS_VERSION_MAJOR >= 4
	ERR_FAIL_COND_MSG(CryptoCore::initialize_crypto_library() != OK, "Failed to initialize PSA Crypto.");
#else
	mbedtls_ctr_drbg_init(&ctr_drbg);
	mbedtls_entropy_init(&entropy);
	int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, nullptr, 0);
	if (ret != 0) {
		ERR_PRINT(" failed\n  ! mbedtls_ctr_drbg_seed returned an error" + itos(ret));
	}
#endif
}

CryptoMbedTLS::~CryptoMbedTLS() {
#if MBEDTLS_VERSION_MAJOR >= 4
	// The PSA backend is shared and released after core crypto users shut down.
#else
	mbedtls_ctr_drbg_free(&ctr_drbg);
	mbedtls_entropy_free(&entropy);
#endif
}

Ref<X509CertificateMbedTLS> CryptoMbedTLS::default_certs;

Ref<X509CertificateMbedTLS> CryptoMbedTLS::get_default_certificates() {
	return default_certs;
}

void CryptoMbedTLS::load_default_certificates(const String &p_path) {
	ERR_FAIL_COND(default_certs.is_valid());

	default_certs = memnew(X509CertificateMbedTLS);
	ERR_FAIL_COND(default_certs.is_null());

	if (!p_path.is_empty()) {
		// Use certs defined in project settings.
		default_certs->load(p_path);
	} else {
		// Try to use system certs otherwise.
		String system_certs = OS::get_singleton()->get_system_ca_certificates();
		if (!system_certs.is_empty()) {
			CharString cs = system_certs.utf8();
			default_certs->load_from_memory((const uint8_t *)cs.get_data(), cs.size());
			print_verbose("Loaded system CA certificates");
		}
#ifdef BUILTIN_CERTS_ENABLED
		else {
			// Use builtin certs if there are no system certs.
			PackedByteArray certs;
			certs.resize(_certs_uncompressed_size + 1);
			const int64_t decompressed_size = Compression::decompress(certs.ptrw(), _certs_uncompressed_size, _certs_compressed, _certs_compressed_size, Compression::MODE_DEFLATE);
			ERR_FAIL_COND_MSG(decompressed_size != _certs_uncompressed_size, "Error decompressing builtin CA certificates. Decompressed size did not match expected size.");
			certs.write[_certs_uncompressed_size] = 0; // Make sure it ends with string terminator
			default_certs->load_from_memory(certs.ptr(), certs.size());
			print_verbose("Loaded builtin CA certificates");
		}
#endif
	}
}

Ref<CryptoKey> CryptoMbedTLS::generate_rsa(int p_bytes) {
#if MBEDTLS_VERSION_MAJOR >= 4
	Ref<CryptoKeyMbedTLS> out;
	out.instantiate();
	ERR_FAIL_COND_V(p_bytes <= 0 || p_bytes > PSA_VENDOR_RSA_MAX_KEY_BITS, nullptr);
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attributes, PSA_KEY_TYPE_RSA_KEY_PAIR);
	psa_set_key_bits(&attributes, p_bytes);
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH | PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
	psa_set_key_algorithm(&attributes, PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_ANY_HASH));
	psa_set_key_enrollment_algorithm(&attributes, PSA_ALG_RSA_PKCS1V15_CRYPT);
	psa_key_id_t id = 0;
	psa_status_t status = psa_generate_key(&attributes, &id);
	psa_reset_key_attributes(&attributes);
	ERR_FAIL_COND_V(status != PSA_SUCCESS, nullptr);
	int ret = mbedtls_pk_copy_from_psa(id, &out->pkey);
	psa_destroy_key(id);
	ERR_FAIL_COND_V(ret != 0, nullptr);
	out->public_only = false;
	return out;
#else
	Ref<CryptoKeyMbedTLS> out;
	out.instantiate();
	int ret = mbedtls_pk_setup(&(out->pkey), mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
	ERR_FAIL_COND_V(ret != 0, nullptr);
	ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(out->pkey), mbedtls_ctr_drbg_random, &ctr_drbg, p_bytes, 65537);
	out->public_only = false;
	ERR_FAIL_COND_V(ret != 0, nullptr);
	return out;
#endif
}

Ref<X509Certificate> CryptoMbedTLS::generate_self_signed_certificate(Ref<CryptoKey> p_key, const String &p_issuer_name, const String &p_not_before, const String &p_not_after) {
	Ref<CryptoKeyMbedTLS> key = static_cast<Ref<CryptoKeyMbedTLS>>(p_key);
	ERR_FAIL_COND_V_MSG(key.is_null(), nullptr, "Invalid private key argument.");
	mbedtls_x509write_cert crt;
	mbedtls_x509write_crt_init(&crt);

	mbedtls_x509write_crt_set_subject_key(&crt, &(key->pkey));
	mbedtls_x509write_crt_set_issuer_key(&crt, &(key->pkey));
	mbedtls_x509write_crt_set_subject_name(&crt, p_issuer_name.utf8().get_data());
	mbedtls_x509write_crt_set_issuer_name(&crt, p_issuer_name.utf8().get_data());
	mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
	mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);

	uint8_t rand_serial[20];
#if MBEDTLS_VERSION_MAJOR >= 4
	if (psa_generate_random(rand_serial, sizeof(rand_serial)) != PSA_SUCCESS) {
		mbedtls_x509write_crt_free(&crt);
		return nullptr;
	}
#else
	mbedtls_ctr_drbg_random(&ctr_drbg, rand_serial, sizeof(rand_serial));
#endif

#if MBEDTLS_VERSION_MAJOR >= 3
	mbedtls_x509write_crt_set_serial_raw(&crt, rand_serial, sizeof(rand_serial));
#else
	mbedtls_mpi serial;
	mbedtls_mpi_init(&serial);
	ERR_FAIL_COND_V(mbedtls_mpi_read_binary(&serial, rand_serial, sizeof(rand_serial)), nullptr);
	mbedtls_x509write_crt_set_serial(&crt, &serial);
#endif

	mbedtls_x509write_crt_set_validity(&crt, p_not_before.utf8().get_data(), p_not_after.utf8().get_data());
	mbedtls_x509write_crt_set_basic_constraints(&crt, 1, -1);
	mbedtls_x509write_crt_set_basic_constraints(&crt, 1, 0);

	unsigned char buf[4096];
	memset(buf, 0, 4096);
#if MBEDTLS_VERSION_MAJOR >= 4
	int ret = mbedtls_x509write_crt_pem(&crt, buf, sizeof(buf));
#else
	int ret = mbedtls_x509write_crt_pem(&crt, buf, 4096, mbedtls_ctr_drbg_random, &ctr_drbg);
#endif
#if MBEDTLS_VERSION_MAJOR < 3
	mbedtls_mpi_free(&serial);
#endif
	mbedtls_x509write_crt_free(&crt);
	ERR_FAIL_COND_V_MSG(ret != 0, nullptr, "Failed to generate certificate: " + itos(ret));
	buf[4095] = '\0'; // Make sure strlen can't fail.

	Ref<X509CertificateMbedTLS> out;
	out.instantiate();
	out->load_from_memory(buf, strlen((char *)buf) + 1); // Use strlen to find correct output size.
	return out;
}

PackedByteArray CryptoMbedTLS::generate_random_bytes(int p_bytes) {
#if MBEDTLS_VERSION_MAJOR >= 4
	ERR_FAIL_COND_V(p_bytes < 0, PackedByteArray());
	PackedByteArray out;
	out.resize(p_bytes);
	ERR_FAIL_COND_V(psa_generate_random(out.ptrw(), out.size()) != PSA_SUCCESS, PackedByteArray());
	return out;
#else
	ERR_FAIL_COND_V(p_bytes < 0, PackedByteArray());
	PackedByteArray out;
	out.resize(p_bytes);
	int left = p_bytes;
	int pos = 0;
	// Ensure we generate random in chunks of no more than MBEDTLS_CTR_DRBG_MAX_REQUEST bytes or mbedtls_ctr_drbg_random will fail.
	while (left > 0) {
		int to_read = MIN(left, MBEDTLS_CTR_DRBG_MAX_REQUEST);
		int ret = mbedtls_ctr_drbg_random(&ctr_drbg, out.ptrw() + pos, to_read);
		ERR_FAIL_COND_V_MSG(ret != 0, PackedByteArray(), vformat("Failed to generate %d random bytes(s). Error: %d.", p_bytes, ret));
		left -= to_read;
		pos += to_read;
	}
	return out;
#endif
}

mbedtls_md_type_t CryptoMbedTLS::md_type_from_hashtype(HashingContext::HashType p_hash_type, int &r_size) {
	switch (p_hash_type) {
		case HashingContext::HASH_MD5:
			r_size = 16;
			return MBEDTLS_MD_MD5;
		case HashingContext::HASH_SHA1:
			r_size = 20;
			return MBEDTLS_MD_SHA1;
		case HashingContext::HASH_SHA256:
			r_size = 32;
			return MBEDTLS_MD_SHA256;
		default:
			r_size = 0;
			ERR_FAIL_V_MSG(MBEDTLS_MD_NONE, "Invalid hash type.");
	}
}

Vector<uint8_t> CryptoMbedTLS::sign(HashingContext::HashType p_hash_type, const Vector<uint8_t> &p_hash, Ref<CryptoKey> p_key) {
	int size;
	mbedtls_md_type_t type = CryptoMbedTLS::md_type_from_hashtype(p_hash_type, size);
	ERR_FAIL_COND_V_MSG(type == MBEDTLS_MD_NONE, Vector<uint8_t>(), "Invalid hash type.");
	ERR_FAIL_COND_V_MSG(p_hash.size() != size, Vector<uint8_t>(), "Invalid hash provided. Size must be " + itos(size));
	Ref<CryptoKeyMbedTLS> key = static_cast<Ref<CryptoKeyMbedTLS>>(p_key);
	ERR_FAIL_COND_V_MSG(key.is_null(), Vector<uint8_t>(), "Invalid key provided.");
	ERR_FAIL_COND_V_MSG(key->is_public_only(), Vector<uint8_t>(), "Invalid key provided. Cannot sign with public_only keys.");
	size_t sig_size = 0;
#if MBEDTLS_VERSION_MAJOR >= 3
	unsigned char buf[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
#else
	unsigned char buf[MBEDTLS_MPI_MAX_SIZE];
#endif
	Vector<uint8_t> out;
	int ret = mbedtls_pk_sign(&(key->pkey), type, p_hash.ptr(), size, buf,
#if MBEDTLS_VERSION_MAJOR >= 3
			sizeof(buf),
#endif
			&sig_size
#if MBEDTLS_VERSION_MAJOR < 4
			,
			mbedtls_ctr_drbg_random, &ctr_drbg
#endif
	);
	ERR_FAIL_COND_V_MSG(ret, out, "Error while signing: " + itos(ret));
	out.resize(sig_size);
	memcpy(out.ptrw(), buf, sig_size);
	return out;
}

bool CryptoMbedTLS::verify(HashingContext::HashType p_hash_type, const Vector<uint8_t> &p_hash, const Vector<uint8_t> &p_signature, Ref<CryptoKey> p_key) {
	int size;
	mbedtls_md_type_t type = CryptoMbedTLS::md_type_from_hashtype(p_hash_type, size);
	ERR_FAIL_COND_V_MSG(type == MBEDTLS_MD_NONE, false, "Invalid hash type.");
	ERR_FAIL_COND_V_MSG(p_hash.size() != size, false, "Invalid hash provided. Size must be " + itos(size));
	Ref<CryptoKeyMbedTLS> key = static_cast<Ref<CryptoKeyMbedTLS>>(p_key);
	ERR_FAIL_COND_V_MSG(key.is_null(), false, "Invalid key provided.");
	return mbedtls_pk_verify(&(key->pkey), type, p_hash.ptr(), size, p_signature.ptr(), p_signature.size()) == 0;
}

Vector<uint8_t> CryptoMbedTLS::encrypt(Ref<CryptoKey> p_key, const Vector<uint8_t> &p_plaintext) {
	Ref<CryptoKeyMbedTLS> key = static_cast<Ref<CryptoKeyMbedTLS>>(p_key);
	ERR_FAIL_COND_V_MSG(key.is_null(), Vector<uint8_t>(), "Invalid key provided.");
	uint8_t buf[1024];
	size_t size;
	Vector<uint8_t> out;
#if MBEDTLS_VERSION_MAJOR >= 4
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t id = 0;
	int ret = mbedtls_pk_get_psa_attributes(&key->pkey, PSA_KEY_USAGE_ENCRYPT, &attributes);
	if (ret == 0) {
		ret = mbedtls_pk_import_into_psa(&key->pkey, &attributes, &id);
	}
	psa_reset_key_attributes(&attributes);
	if (ret == 0) {
		ret = psa_asymmetric_encrypt(id, PSA_ALG_RSA_PKCS1V15_CRYPT, p_plaintext.ptr(), p_plaintext.size(), nullptr, 0, buf, sizeof(buf), &size);
	}
	if (id) {
		psa_destroy_key(id);
	}
#else
	int ret = mbedtls_pk_encrypt(&(key->pkey), p_plaintext.ptr(), p_plaintext.size(), buf, &size, sizeof(buf), mbedtls_ctr_drbg_random, &ctr_drbg);
#endif
	ERR_FAIL_COND_V_MSG(ret, out, "Error while encrypting: " + itos(ret));
	out.resize(size);
	memcpy(out.ptrw(), buf, size);
	return out;
}

Vector<uint8_t> CryptoMbedTLS::decrypt(Ref<CryptoKey> p_key, const Vector<uint8_t> &p_ciphertext) {
	Ref<CryptoKeyMbedTLS> key = static_cast<Ref<CryptoKeyMbedTLS>>(p_key);
	ERR_FAIL_COND_V_MSG(key.is_null(), Vector<uint8_t>(), "Invalid key provided.");
	ERR_FAIL_COND_V_MSG(key->is_public_only(), Vector<uint8_t>(), "Invalid key provided. Cannot decrypt using a public_only key.");
	uint8_t buf[2048];
	size_t size;
	Vector<uint8_t> out;
#if MBEDTLS_VERSION_MAJOR >= 4
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t id = 0;
	int ret = mbedtls_pk_get_psa_attributes(&key->pkey, PSA_KEY_USAGE_DECRYPT, &attributes);
	if (ret == 0) {
		ret = mbedtls_pk_import_into_psa(&key->pkey, &attributes, &id);
	}
	psa_reset_key_attributes(&attributes);
	if (ret == 0) {
		ret = psa_asymmetric_decrypt(id, PSA_ALG_RSA_PKCS1V15_CRYPT, p_ciphertext.ptr(), p_ciphertext.size(), nullptr, 0, buf, sizeof(buf), &size);
	}
	if (id) {
		psa_destroy_key(id);
	}
#else
	int ret = mbedtls_pk_decrypt(&(key->pkey), p_ciphertext.ptr(), p_ciphertext.size(), buf, &size, sizeof(buf), mbedtls_ctr_drbg_random, &ctr_drbg);
#endif
	ERR_FAIL_COND_V_MSG(ret, out, "Error while decrypting: " + itos(ret));
	out.resize(size);
	memcpy(out.ptrw(), buf, size);
	return out;
}
