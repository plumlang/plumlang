/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * TLS for the net module, by the vendored Mbed TLS (vendor/mbedtls).
 *
 * Mbed TLS reads and writes the socket through two small functions here,
 * which never wait: when the socket isn't ready they say so
 * (MBEDTLS_ERR_SSL_WANT_READ / WANT_WRITE), and the net module waits for
 * it the way it does for plain sockets, letting other tasks run.
 *
 * The system's trusted CA certificates are loaded once per run of a
 * program, the first time a connection needs them, and kept with the
 * program (plum_native_program_data).
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "stdlib/tls.h"

#if defined(PLUM_NO_TLS)
/*
 * Built without Mbed TLS (make wasm, where there are no sockets anyway):
 * a TLS connection or listener fails with this reason, which the net
 * module throws as a NetError.
 */
#include <stdio.h>

#define NO_TLS "TLS isn't available in this build of plum (built without Mbed TLS)"

PlumTls *plum_tls_client(PlumNative *n, intptr_t fd, const char *host, bool verify,
                         const char *ca_file, char *err) {
    (void)n;
    (void)fd;
    (void)host;
    (void)verify;
    (void)ca_file;
    snprintf(err, PLUM_TLS_ERR_SIZE, "%s", NO_TLS);
    return NULL;
}

PlumTlsServer *plum_tls_server_new(const char *cert_file, const char *key_file, char *err) {
    (void)cert_file;
    (void)key_file;
    snprintf(err, PLUM_TLS_ERR_SIZE, "%s", NO_TLS);
    return NULL;
}

void plum_tls_server_release(PlumTlsServer *s) {
    (void)s;
}

PlumTls *plum_tls_accept(PlumTlsServer *s, intptr_t fd, char *err) {
    (void)s;
    (void)fd;
    snprintf(err, PLUM_TLS_ERR_SIZE, "%s", NO_TLS);
    return NULL;
}

bool plum_tls_ready(const PlumTls *t) {
    (void)t;
    return false;
}

int plum_tls_handshake(PlumTls *t, char *err) {
    (void)t;
    snprintf(err, PLUM_TLS_ERR_SIZE, "%s", NO_TLS);
    return PLUM_TLS_FAILED;
}

long plum_tls_read(PlumTls *t, char *buf, size_t len, char *err) {
    (void)t;
    (void)buf;
    (void)len;
    snprintf(err, PLUM_TLS_ERR_SIZE, "%s", NO_TLS);
    return PLUM_TLS_FAILED;
}

long plum_tls_write(PlumTls *t, const char *buf, size_t len, char *err) {
    (void)t;
    (void)buf;
    (void)len;
    snprintf(err, PLUM_TLS_ERR_SIZE, "%s", NO_TLS);
    return PLUM_TLS_FAILED;
}

void plum_tls_free(PlumTls *t) {
    (void)t;
}

#else
#include "util/alloc.h"

#include "mbedtls/error.h"
#include "mbedtls/pem.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
/* in this order: winsock2.h before windows.h, wincrypt.h after it */
#include <winsock2.h>

#include <windows.h>

#include <wincrypt.h>
typedef SOCKET tls_sock;
#define SEND_FLAGS 0
static int sock_error(void) {
    return WSAGetLastError();
}
static bool sock_would_block(int err) {
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEINTR;
}
static void sock_error_text(int err, char *buf, size_t size) {
    if (FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD)err,
                       0, buf, (DWORD)size, NULL) == 0) {
        snprintf(buf, size, "network error %d", err);
    }
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == '.')) {
        buf[--len] = '\0';
    }
}
#else
#include <sys/socket.h>
#include <sys/types.h>
#if defined(MSG_NOSIGNAL)
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif
typedef int tls_sock;
static int sock_error(void) {
    return errno;
}
static bool sock_would_block(int err) {
    return err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
}
static void sock_error_text(int err, char *buf, size_t size) {
    snprintf(buf, size, "%s", strerror(err));
}
#endif

/* What the socket functions return when the socket failed (err in sys_err). */
#define SOCKET_FAILED (-0x004C)

struct PlumTlsServer {
    int refs;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt chain;
    mbedtls_pk_context key;
};

struct PlumTls {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf; /* a client's own */
    mbedtls_x509_crt ca;     /* a client's ca_file */
    bool client;
    bool own_ca;
    bool no_cas;           /* verifying, but the system has no trusted CAs */
    bool done;             /* the handshake is over */
    PlumTlsServer *server; /* a server's side: the listener's certificate */
    intptr_t fd;
    int sys_err; /* the socket's error, after SOCKET_FAILED */
};

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

/* What mbedtls_strerror doesn't know: the crypto library's key and PEM codes. */
static const char *crypto_text(int code) {
    switch (code) {
    case MBEDTLS_ERR_PK_FILE_IO_ERROR:
        return "the file can't be read";
    case MBEDTLS_ERR_PK_KEY_INVALID_VERSION:
    case MBEDTLS_ERR_PK_KEY_INVALID_FORMAT:
    case MBEDTLS_ERR_PEM_INVALID_DATA:
        return "not a private key in PEM or DER form";
    case MBEDTLS_ERR_PK_UNKNOWN_PK_ALG:
    case MBEDTLS_ERR_PK_UNKNOWN_NAMED_CURVE:
    case MBEDTLS_ERR_PK_FEATURE_UNAVAILABLE:
        return "a kind of key that isn't supported";
    case MBEDTLS_ERR_PK_PASSWORD_REQUIRED:
    case MBEDTLS_ERR_PK_PASSWORD_MISMATCH:
    case MBEDTLS_ERR_PEM_PASSWORD_REQUIRED:
    case MBEDTLS_ERR_PEM_PASSWORD_MISMATCH:
        return "the key is encrypted with a password; give it unencrypted";
    case MBEDTLS_ERR_PK_TYPE_MISMATCH:
        return "the key doesn't fit the certificate";
    case MBEDTLS_ERR_PEM_NO_HEADER_FOOTER_PRESENT:
        return "no PEM data in the file";
    case PSA_ERROR_INSUFFICIENT_MEMORY:
        return "out of memory";
    default:
        return NULL;
    }
}

/* Mbed TLS's text for `code`, without its "SSL - " prefixes. */
static void library_text(int code, char *out, size_t size) {
    const char *known = crypto_text(code);
    if (known != NULL) {
        snprintf(out, size, "%s", known);
        return;
    }
    char raw[PLUM_TLS_ERR_SIZE];
    mbedtls_strerror(code, raw, sizeof raw);
    if (strncmp(raw, "UNKNOWN ERROR CODE", 18) == 0) {
        snprintf(out, size, "error -0x%04X", (unsigned)-code);
        return;
    }
    size_t o = 0;
    const char *p = raw;
    while (*p != '\0' && o + 1 < size) {
        /* "WORD - " at the start of each part goes */
        const char *dash = strstr(p, " - ");
        if (dash != NULL && dash - p <= 12) {
            bool word = true;
            for (const char *q = p; q < dash; q++) {
                word = word && (isupper((unsigned char)*q) || isdigit((unsigned char)*q) ||
                                *q == '_' || *q == '/');
            }
            if (word) {
                p = dash + 3;
            }
        }
        const char *end = strstr(p, " : ");
        size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
        if (o > 0) {
            o += (size_t)snprintf(out + o, size - o, ": ");
        }
        if (o + 1 < size) {
            o += (size_t)snprintf(out + o, size - o, "%.*s", (int)len, p);
        }
        if (o >= size) {
            o = size - 1;
        }
        p = end != NULL ? end + 3 : p + len;
    }
    out[o] = '\0';
    if (out[0] != '\0' && isupper((unsigned char)out[0]) && islower((unsigned char)out[1])) {
        out[0] = (char)tolower((unsigned char)out[0]);
    }
}

static void verify_text(PlumTls *t, char *err) {
    char info[512];
    uint32_t flags = mbedtls_ssl_get_verify_result(&t->ssl);
    if (mbedtls_x509_crt_verify_info(info, sizeof info, "", flags) < 0) {
        info[0] = '\0';
    }
    /* one line per reason: "reason; reason" */
    size_t o = (size_t)snprintf(err, PLUM_TLS_ERR_SIZE, "certificate verify failed");
    const char *p = info;
    bool first = true;
    while (*p != '\0' && o + 1 < PLUM_TLS_ERR_SIZE) {
        const char *nl = strchr(p, '\n');
        size_t len = nl != NULL ? (size_t)(nl - p) : strlen(p);
        if (len > 0) {
            char line[256];
            snprintf(line, sizeof line, "%.*s", (int)len, p);
            if (isupper((unsigned char)line[0]) && islower((unsigned char)line[1])) {
                line[0] = (char)tolower((unsigned char)line[0]);
            }
            o +=
                (size_t)snprintf(err + o, PLUM_TLS_ERR_SIZE - o, "%s%s", first ? ": " : "; ", line);
            first = false;
        }
        p = nl != NULL ? nl + 1 : p + len;
    }
    if (t->no_cas && o + 1 < PLUM_TLS_ERR_SIZE) {
        snprintf(err + o, PLUM_TLS_ERR_SIZE - o,
                 " (no trusted CA certificates were found on this system; set SSL_CERT_FILE or "
                 "pass ca_file)");
    }
}

/* Why `code` happened, into err; returns what the caller returns. */
static int failed(PlumTls *t, int code, char *err) {
    if (code == MBEDTLS_ERR_SSL_WANT_READ) {
        return PLUM_TLS_WANT_READ;
    }
    if (code == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return PLUM_TLS_WANT_WRITE;
    }
    if (code == SOCKET_FAILED && t->sys_err != 0) {
        sock_error_text(t->sys_err, err, PLUM_TLS_ERR_SIZE);
    } else if (code == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        verify_text(t, err);
    } else if (code == MBEDTLS_ERR_SSL_CA_CHAIN_REQUIRED) {
        snprintf(err, PLUM_TLS_ERR_SIZE,
                 "certificate verify failed: no trusted CA certificates were found on this "
                 "system; set SSL_CERT_FILE or pass ca_file");
    } else if (code == MBEDTLS_ERR_SSL_CONN_EOF) {
        snprintf(err, PLUM_TLS_ERR_SIZE, "the other end closed the connection %s",
                 t->done ? "unexpectedly" : "during the TLS handshake");
    } else {
        char text[200];
        library_text(code, text, sizeof text);
        snprintf(err, PLUM_TLS_ERR_SIZE, "%s%s", t->done ? "" : "TLS handshake failed: ", text);
    }
    return PLUM_TLS_FAILED;
}

/* ------------------------------------------------------------------ */
/* The socket, as Mbed TLS sees it                                      */
/* ------------------------------------------------------------------ */

static int sock_send(void *ctx, const unsigned char *buf, size_t len) {
    PlumTls *t = ctx;
    int r = (int)send((tls_sock)t->fd, (const char *)buf, (int)(len > 1 << 30 ? 1 << 30 : len),
                      SEND_FLAGS);
    if (r >= 0) {
        return r;
    }
    int err = sock_error();
    if (sock_would_block(err)) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    t->sys_err = err;
    return SOCKET_FAILED;
}

static int sock_recv(void *ctx, unsigned char *buf, size_t len) {
    PlumTls *t = ctx;
    int r = (int)recv((tls_sock)t->fd, (char *)buf, (int)(len > 1 << 30 ? 1 << 30 : len), 0);
    if (r >= 0) {
        return r; /* 0: the other end closed */
    }
    int err = sock_error();
    if (sock_would_block(err)) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    t->sys_err = err;
    return SOCKET_FAILED;
}

/* ------------------------------------------------------------------ */
/* Trusted CA certificates                                             */
/* ------------------------------------------------------------------ */

typedef struct Trust {
    mbedtls_x509_crt cas;
    int count;
} Trust;

static void trust_free(void *p) {
    Trust *t = p;
    mbedtls_x509_crt_free(&t->cas);
    plum_free(t);
}

static int chain_length(const mbedtls_x509_crt *c) {
    int count = 0;
    for (; c != NULL && c->raw.len > 0; c = c->next) {
        count++;
    }
    return count;
}

/* Can `path` be opened? If not, why, into text. */
static bool readable(const char *path, char *text, size_t size) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        snprintf(text, size, "%s", strerror(errno));
        return false;
    }
    fclose(f);
    return true;
}

/* The certificates in a PEM (or DER) file, added to `chain`: how many it has now. */
static int load_file(mbedtls_x509_crt *chain, const char *path) {
    mbedtls_x509_crt_parse_file(chain, path); /* what it can read, even if not all */
    return chain_length(chain);
}

static void load_system(Trust *t) {
    const char *file = getenv("SSL_CERT_FILE");
    if (file != NULL && file[0] != '\0') {
        t->count = load_file(&t->cas, file);
        return;
    }
#if defined(_WIN32)
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (store != NULL) {
        PCCERT_CONTEXT c = NULL;
        while ((c = CertEnumCertificatesInStore(store, c)) != NULL) {
            if ((c->dwCertEncodingType & X509_ASN_ENCODING) != 0) {
                mbedtls_x509_crt_parse_der(&t->cas, c->pbCertEncoded, c->cbCertEncoded);
            }
        }
        CertCloseStore(store, 0);
    }
    t->count = chain_length(&t->cas);
#else
    static const char *const bundles[] = {
        "/etc/ssl/certs/ca-certificates.crt", /* Debian, Ubuntu, Arch, Alpine */
        "/etc/pki/tls/certs/ca-bundle.crt",   /* Fedora, RHEL */
        "/etc/ssl/ca-bundle.pem",             /* openSUSE */
        "/etc/ssl/cert.pem",                  /* macOS, the BSDs, Alpine */
    };
    for (size_t i = 0; i < sizeof bundles / sizeof bundles[0]; i++) {
        t->count = load_file(&t->cas, bundles[i]);
        if (t->count > 0) {
            return;
        }
    }
#endif
}

/* The key the program keeps its trusted CAs under: an address of our own. */
static const char trust_key = 0;

static Trust *system_trust(PlumNative *n) {
    void **slot = plum_native_program_data(n, &trust_key, trust_free);
    if (*slot == NULL) {
        Trust *t = plum_alloc_zeroed(1, sizeof *t);
        mbedtls_x509_crt_init(&t->cas);
        load_system(t);
        *slot = t;
    }
    return *slot;
}

static bool crypto_start(char *err) {
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        snprintf(err, PLUM_TLS_ERR_SIZE, "the crypto library didn't start (PSA error %d)",
                 (int)status);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Sessions                                                            */
/* ------------------------------------------------------------------ */

static PlumTls *session_new(intptr_t fd) {
    PlumTls *t = plum_alloc_zeroed(1, sizeof *t);
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_x509_crt_init(&t->ca);
    t->fd = fd;
    return t;
}

PlumTls *plum_tls_client(PlumNative *n, intptr_t fd, const char *host, bool verify,
                         const char *ca_file, char *err) {
    if (!crypto_start(err)) {
        return NULL;
    }
    PlumTls *t = session_new(fd);
    t->client = true;
    int r = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (r == 0) {
        mbedtls_ssl_conf_session_tickets(&t->conf, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
        if (!verify) {
            mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE);
        } else if (ca_file[0] != '\0') {
            t->own_ca = true;
            char text[200];
            if (!readable(ca_file, text, sizeof text)) {
                snprintf(err, PLUM_TLS_ERR_SIZE, "can't read the CA file '%.80s': %s", ca_file,
                         text);
                plum_tls_free(t);
                return NULL;
            }
            if (load_file(&t->ca, ca_file) == 0) {
                snprintf(err, PLUM_TLS_ERR_SIZE, "no certificates could be read from '%.200s'",
                         ca_file);
                plum_tls_free(t);
                return NULL;
            }
            mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
            mbedtls_ssl_conf_ca_chain(&t->conf, &t->ca, NULL);
        } else {
            Trust *trust = system_trust(n);
            mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
            if (trust->count > 0) {
                mbedtls_ssl_conf_ca_chain(&t->conf, &trust->cas, NULL);
            } else {
                t->no_cas = true;
            }
        }
        r = mbedtls_ssl_setup(&t->ssl, &t->conf);
    }
    if (r == 0) {
        r = mbedtls_ssl_set_hostname(&t->ssl, host);
    }
    if (r != 0) {
        failed(t, r, err);
        plum_tls_free(t);
        return NULL;
    }
    mbedtls_ssl_set_bio(&t->ssl, t, sock_send, sock_recv, NULL);
    return t;
}

PlumTlsServer *plum_tls_server_new(const char *cert_file, const char *key_file, char *err) {
    if (!crypto_start(err)) {
        return NULL;
    }
    PlumTlsServer *s = plum_alloc_zeroed(1, sizeof *s);
    s->refs = 1;
    mbedtls_ssl_config_init(&s->conf);
    mbedtls_x509_crt_init(&s->chain);
    mbedtls_pk_init(&s->key);
    char text[200];
    int r = 0;
    bool ok = readable(cert_file, text, sizeof text);
    if (ok) {
        r = mbedtls_x509_crt_parse_file(&s->chain, cert_file);
        ok = r >= 0 && chain_length(&s->chain) > 0;
        if (!ok) {
            library_text(r < 0 ? r : MBEDTLS_ERR_X509_CERT_UNKNOWN_FORMAT, text, sizeof text);
        }
    }
    if (!ok) {
        snprintf(err, PLUM_TLS_ERR_SIZE, "can't read the certificate '%.80s': %s", cert_file, text);
        plum_tls_server_release(s);
        return NULL;
    }
    ok = readable(key_file, text, sizeof text);
    if (ok) {
        r = mbedtls_pk_parse_keyfile(&s->key, key_file, NULL);
        if (r == 0 && mbedtls_pk_check_pair(&s->chain.pk, &s->key) != 0) {
            r = MBEDTLS_ERR_PK_TYPE_MISMATCH;
        }
        ok = r == 0;
        if (!ok) {
            library_text(r, text, sizeof text);
        }
    }
    if (!ok) {
        snprintf(err, PLUM_TLS_ERR_SIZE, "can't read the private key '%.80s': %s", key_file, text);
        plum_tls_server_release(s);
        return NULL;
    }
    r = mbedtls_ssl_config_defaults(&s->conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
    if (r == 0) {
        r = mbedtls_ssl_conf_own_cert(&s->conf, &s->chain, &s->key);
    }
    if (r != 0) {
        library_text(r, text, sizeof text);
        snprintf(err, PLUM_TLS_ERR_SIZE, "can't use the certificate '%.80s': %s", cert_file, text);
        plum_tls_server_release(s);
        return NULL;
    }
    return s;
}

void plum_tls_server_release(PlumTlsServer *s) {
    if (s == NULL || --s->refs > 0) {
        return;
    }
    mbedtls_ssl_config_free(&s->conf);
    mbedtls_x509_crt_free(&s->chain);
    mbedtls_pk_free(&s->key);
    plum_free(s);
}

PlumTls *plum_tls_accept(PlumTlsServer *s, intptr_t fd, char *err) {
    PlumTls *t = session_new(fd);
    int r = mbedtls_ssl_setup(&t->ssl, &s->conf);
    if (r != 0) {
        failed(t, r, err);
        plum_tls_free(t);
        return NULL;
    }
    s->refs++;
    t->server = s;
    mbedtls_ssl_set_bio(&t->ssl, t, sock_send, sock_recv, NULL);
    return t;
}

bool plum_tls_ready(const PlumTls *t) {
    return t->done;
}

int plum_tls_handshake(PlumTls *t, char *err) {
    if (t->done) {
        return 0;
    }
    int r = mbedtls_ssl_handshake(&t->ssl);
    if (r == 0) {
        t->done = true;
        return 0;
    }
    return failed(t, r, err);
}

long plum_tls_read(PlumTls *t, char *buf, size_t len, char *err) {
    if (len > 1 << 20) {
        len = 1 << 20;
    }
    for (;;) {
        int r = mbedtls_ssl_read(&t->ssl, (unsigned char *)buf, len);
        if (r > 0) {
            t->done = true;
            return r;
        }
        if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
            (r == MBEDTLS_ERR_SSL_CONN_EOF && t->done)) {
            return 0; /* the end (many servers just close, without a close_notify) */
        }
        if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET ||
            r == MBEDTLS_ERR_SSL_RECEIVED_EARLY_DATA) {
            continue;
        }
        return failed(t, r, err);
    }
}

long plum_tls_write(PlumTls *t, const char *buf, size_t len, char *err) {
    for (;;) {
        int r = mbedtls_ssl_write(&t->ssl, (const unsigned char *)buf, len);
        if (r >= 0) {
            t->done = true;
            return r;
        }
        if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET ||
            r == MBEDTLS_ERR_SSL_RECEIVED_EARLY_DATA) {
            continue;
        }
        return failed(t, r, err);
    }
}

void plum_tls_free(PlumTls *t) {
    if (t == NULL) {
        return;
    }
    if (t->done && t->sys_err == 0) {
        mbedtls_ssl_close_notify(&t->ssl); /* if the socket takes it now; no waiting */
    }
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_x509_crt_free(&t->ca);
    plum_tls_server_release(t->server);
    plum_free(t);
}
#endif /* PLUM_NO_TLS */
