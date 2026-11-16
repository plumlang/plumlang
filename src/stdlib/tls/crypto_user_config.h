/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The TF-PSA-Crypto options plum builds with: the defaults of
 * vendor/mbedtls/tf-psa-crypto/include/psa/crypto_config.h, less what the
 * usual TLS 1.2/1.3 cipher suites don't need. Read after the default
 * configuration (TF_PSA_CRYPTO_USER_CONFIG_FILE), so it only takes options
 * away. What stays: AES-GCM, AES-CCM, AES-CBC, ChaCha20-Poly1305, SHA-1/2,
 * HMAC, HKDF, the TLS 1.2 PRF, ECDH/ECDSA on P-256/384/521 and X25519/X448,
 * and RSA (PKCS#1 v1.5 and PSS signatures).
 */
#ifndef PLUM_CRYPTO_USER_CONFIG_H
#define PLUM_CRYPTO_USER_CONFIG_H

/* Keys live in memory only: never write a key store to the disk. */
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C

#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_LMS_C
#undef MBEDTLS_NIST_KW_C
#undef MBEDTLS_PKCS5_C
#undef MBEDTLS_PEM_WRITE_C

/* algorithms no TLS cipher suite here uses */
#undef PSA_WANT_ALG_CCM_STAR_NO_TAG
#undef PSA_WANT_ALG_CMAC
#undef PSA_WANT_ALG_CFB
#undef PSA_WANT_ALG_OFB
#undef PSA_WANT_ALG_CTR
#undef PSA_WANT_ALG_JPAKE
#undef PSA_WANT_ALG_TLS12_ECJPAKE_TO_PMS
#undef PSA_WANT_ALG_MD5
#undef PSA_WANT_ALG_RIPEMD160
#undef PSA_WANT_ALG_PBKDF2_HMAC
#undef PSA_WANT_ALG_PBKDF2_AES_CMAC_PRF_128
#undef PSA_WANT_ALG_SHA3_224
#undef PSA_WANT_ALG_SHA3_256
#undef PSA_WANT_ALG_SHA3_384
#undef PSA_WANT_ALG_SHA3_512
#undef PSA_WANT_ALG_SHAKE128
#undef PSA_WANT_ALG_SHAKE256
#undef PSA_WANT_ALG_TLS12_PSK_TO_MS
#undef PSA_WANT_KEY_TYPE_ARIA
#undef PSA_WANT_KEY_TYPE_CAMELLIA
#undef PSA_WANT_KEY_TYPE_PASSWORD
#undef PSA_WANT_KEY_TYPE_PASSWORD_HASH

/* finite-field Diffie-Hellman: TLS 1.3 servers almost never pick it */
#undef PSA_WANT_ALG_FFDH
#undef PSA_WANT_DH_RFC7919_2048
#undef PSA_WANT_DH_RFC7919_3072
#undef PSA_WANT_DH_RFC7919_4096
#undef PSA_WANT_DH_RFC7919_6144
#undef PSA_WANT_DH_RFC7919_8192
#undef PSA_WANT_KEY_TYPE_DH_PUBLIC_KEY
#undef PSA_WANT_KEY_TYPE_DH_KEY_PAIR_BASIC
#undef PSA_WANT_KEY_TYPE_DH_KEY_PAIR_IMPORT
#undef PSA_WANT_KEY_TYPE_DH_KEY_PAIR_EXPORT
#undef PSA_WANT_KEY_TYPE_DH_KEY_PAIR_GENERATE

/* curves outside the usual TLS set */
#undef PSA_WANT_ECC_BRAINPOOL_P_R1_256
#undef PSA_WANT_ECC_BRAINPOOL_P_R1_384
#undef PSA_WANT_ECC_BRAINPOOL_P_R1_512
#undef PSA_WANT_ECC_SECP_K1_256

/* plum never makes RSA keys */
#undef PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_GENERATE

/* The browser (make wasm): plum gives the entropy itself (src/stdlib/hash.c). */
#if defined(__EMSCRIPTEN__)
#undef MBEDTLS_PSA_BUILTIN_GET_ENTROPY
#define MBEDTLS_PSA_DRIVER_GET_ENTROPY
#endif

#endif /* PLUM_CRYPTO_USER_CONFIG_H */
