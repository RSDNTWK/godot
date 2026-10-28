#ifndef GODOT_PSA_CRYPTO_CONFIG_H
#define GODOT_PSA_CRYPTO_CONFIG_H

#include "platform_config.h"

#ifdef GODOT_PSA_CRYPTO_INCLUDE_H
#include GODOT_PSA_CRYPTO_INCLUDE_H
#else
#include <psa/crypto_config.h>
#define MBEDTLS_DEPRECATED_REMOVED

// Use the same cryptographic operating-system entropy source as CryptoCore.
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
#undef MBEDTLS_PSA_BUILTIN_GET_ENTROPY
#undef MBEDTLS_CTR_DRBG_C
#undef MBEDTLS_HMAC_DRBG_C

// Godot uses volatile keys and its own file access, not PSA's file key store.
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C

// Retain the existing exclusion of DES and finite-field Diffie-Hellman.
#undef PSA_WANT_KEY_TYPE_DES
#undef PSA_WANT_KEY_TYPE_DH_PUBLIC_KEY
#undef PSA_WANT_KEY_TYPE_DH_KEY_PAIR_BASIC
#undef PSA_WANT_KEY_TYPE_DH_KEY_PAIR_IMPORT
#undef PSA_WANT_KEY_TYPE_DH_KEY_PAIR_EXPORT
#undef PSA_WANT_KEY_TYPE_DH_KEY_PAIR_GENERATE
#undef PSA_WANT_ALG_FFDH

#ifdef THREADS_ENABLED
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT
#define GODOT_MBEDTLS_THREADING_ALT
#endif

#if !(defined(__linux__) && defined(__aarch64__))
#undef MBEDTLS_AESCE_C
#endif
#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
#undef MBEDTLS_HAVE_ASM
#undef MBEDTLS_AESNI_C
#endif
#endif
#endif // GODOT_PSA_CRYPTO_INCLUDE_H

#endif // GODOT_PSA_CRYPTO_CONFIG_H
