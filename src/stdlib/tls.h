/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * TLS over the net module's sockets, by the vendored Mbed TLS
 * (vendor/mbedtls). Nothing here waits: an operation that needs the
 * socket returns PLUM_TLS_WANT_READ or PLUM_TLS_WANT_WRITE, the caller
 * waits for that (plum_native_wait_io) and makes the same call again.
 * Messages are written into `err` (at least PLUM_TLS_ERR_SIZE bytes).
 */
#ifndef PLUM_STDLIB_TLS_H
#define PLUM_STDLIB_TLS_H

#include "stdlib/native.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PLUM_TLS_ERR_SIZE 320

enum {
    PLUM_TLS_WANT_READ = -1,
    PLUM_TLS_WANT_WRITE = -2,
    PLUM_TLS_FAILED = -3,
};

/* One TLS session over a socket. */
typedef struct PlumTls PlumTls;
/* A listener's certificate and key, shared with the connections it accepts. */
typedef struct PlumTlsServer PlumTlsServer;

/*
 * A client session over socket `fd` to `host`, checking the server's
 * certificate against `ca_file` ("" for the system's trusted CAs) and
 * `host`, unless `verify` is false. NULL after writing why into err.
 */
PlumTls *plum_tls_client(PlumNative *n, intptr_t fd, const char *host, bool verify,
                         const char *ca_file, char *err);

/* A server's certificate chain and private key, from PEM files. */
PlumTlsServer *plum_tls_server_new(const char *cert_file, const char *key_file, char *err);
void plum_tls_server_release(PlumTlsServer *s);

/* The server's side of a connection accepted on socket `fd`. */
PlumTls *plum_tls_accept(PlumTlsServer *s, intptr_t fd, char *err);

/* Has the handshake finished? */
bool plum_tls_ready(const PlumTls *t);

/* Go on with the handshake: 0 once it's done, or one of the codes above. */
int plum_tls_handshake(PlumTls *t, char *err);

/* Bytes read (> 0), 0 at the end of the stream, or one of the codes above. */
long plum_tls_read(PlumTls *t, char *buf, size_t len, char *err);

/* Bytes written (> 0), or one of the codes above. */
long plum_tls_write(PlumTls *t, const char *buf, size_t len, char *err);

/* Say goodbye (if the socket takes it at once) and free the session. The
 * socket itself stays open. */
void plum_tls_free(PlumTls *t);

#endif /* PLUM_STDLIB_TLS_H */
