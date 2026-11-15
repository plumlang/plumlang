/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The Mbed TLS (TLS and X.509) options plum builds with: the defaults of
 * vendor/mbedtls/include/mbedtls/mbedtls_config.h, less what a TLS 1.2/1.3
 * client and server don't use. Read after the default configuration
 * (MBEDTLS_USER_CONFIG_FILE), so it only takes options away.
 */
#ifndef PLUM_MBEDTLS_USER_CONFIG_H
#define PLUM_MBEDTLS_USER_CONFIG_H

/* plum has its own sockets and its own clock */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C

#undef MBEDTLS_DEBUG_C
#undef MBEDTLS_VERSION_FEATURES

/* no DTLS */
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_COOKIE_C

/* no pre-shared keys in TLS 1.2 */
#undef MBEDTLS_KEY_EXCHANGE_ECDHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_PSK_ENABLED

/* no server-side session cache or tickets, renegotiation, or saved contexts */
#undef MBEDTLS_SSL_CACHE_C
#undef MBEDTLS_SSL_TICKET_C
#undef MBEDTLS_SSL_RENEGOTIATION
#undef MBEDTLS_SSL_CONTEXT_SERIALIZATION
#undef MBEDTLS_SSL_KEYING_MATERIAL_EXPORT

/* X.509: reading certificates only */
#undef MBEDTLS_PKCS7_C
#undef MBEDTLS_X509_CREATE_C
#undef MBEDTLS_X509_CRT_WRITE_C
#undef MBEDTLS_X509_CSR_PARSE_C
#undef MBEDTLS_X509_CSR_WRITE_C
#undef MBEDTLS_X509_CRL_PARSE_C

#endif /* PLUM_MBEDTLS_USER_CONFIG_H */
