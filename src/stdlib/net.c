/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The net module: TCP listeners and connections, UDP sockets, and DNS.
 *
 * Every socket is non-blocking. When an operation would wait, the task
 * hands its socket to the scheduler (plum_native_wait_io) and other tasks
 * run; the call is made again once the socket is ready. Each call that
 * waits takes a `timeout` in seconds (0: no limit) and throws NetError
 * when it runs out. Name lookups (getaddrinfo) wait without letting other
 * tasks run.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "stdlib/native.h"

#include "util/alloc.h"
#include "util/array.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCKET INVALID_SOCKET
static int last_error(void) {
    return WSAGetLastError();
}
static bool would_block(int err) {
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY;
}
static void close_socket(sock_t s) {
    closesocket(s);
}
/* A datagram bigger than the buffer: Windows says so, but fills the buffer. */
static bool truncated(int err) {
    return err == WSAEMSGSIZE;
}
static bool set_nonblocking(sock_t s) {
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
}
/* Winsock needs starting once per process; it counts the calls. */
static void net_start(void) {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
}
static const char *error_text(int err, char *buf, size_t size) {
    if (FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD)err,
                       0, buf, (DWORD)size, NULL) == 0) {
        snprintf(buf, size, "network error %d", err);
    }
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == '.')) {
        buf[--len] = '\0';
    }
    return buf;
}
#define SEND_FLAGS 0
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCKET (-1)
static int last_error(void) {
    return errno;
}
static bool would_block(int err) {
    return err == EAGAIN || err == EWOULDBLOCK || err == EINPROGRESS || err == EINTR;
}
static void close_socket(sock_t s) {
    close(s);
}
static bool truncated(int err) {
    (void)err;
    return false; /* POSIX just cuts the datagram short */
}
static bool set_nonblocking(sock_t s) {
    int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
static void net_start(void) {
    /* a write to a closed connection is an error to report, not a signal
     * that ends the program (macOS has no MSG_NOSIGNAL) */
    signal(SIGPIPE, SIG_IGN);
}
static const char *error_text(int err, char *buf, size_t size) {
    snprintf(buf, size, "%s", strerror(err));
    return buf;
}
#if defined(MSG_NOSIGNAL)
#define SEND_FLAGS MSG_NOSIGNAL /* a closed peer is an error, not a signal that ends us */
#else
#define SEND_FLAGS 0
#endif
#endif

#define ARGS PlumNative *n, const PlumValue *args, int argc, PlumValue *result

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static bool net_fail(PlumNative *n, const char *what, int err) {
    char text[256];
    return plum_native_throw(n, PLUM_ERROR_NET, "%s: %s", what, error_text(err, text, sizeof text));
}

static double timeout_arg(const PlumValue *args, int argc, int at) {
    if (argc <= at || args[at].type == PLUM_VAL_NONE) {
        return 0;
    }
    return args[at].type == PLUM_VAL_INT ? (double)args[at].as.i : args[at].as.f;
}

static bool check_timeout(PlumNative *n, double timeout) {
    if (timeout < 0) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "timeout must be 0 (no limit) or more");
    }
    return true;
}

static bool check_port(PlumNative *n, int64_t port, bool zero_ok) {
    if (port < (zero_ok ? 0 : 1) || port > 65535) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "port %lld is out of range; it must be %s to 65535",
                                 (long long)port, zero_ok ? "0" : "1");
    }
    return true;
}

static const char *str_arg(PlumValue v) {
    return plum_as_string(v)->chars;
}

/* "host:port" (or "[v6]:port") of an address. */
static void address_text(const struct sockaddr *sa, char *buf, size_t size) {
    char host[64] = "?";
    int port = 0;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)(const void *)sa;
        inet_ntop(AF_INET, &in->sin_addr, host, sizeof host);
        port = ntohs(in->sin_port);
        snprintf(buf, size, "%s:%d", host, port);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)(const void *)sa;
        inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof host);
        port = ntohs(in6->sin6_port);
        snprintf(buf, size, "[%s]:%d", host, port);
    } else {
        snprintf(buf, size, "?");
    }
}

static int local_port(sock_t s) {
    struct sockaddr_storage addr;
    socklen_t len = sizeof addr;
    if (getsockname(s, (struct sockaddr *)&addr, &len) != 0) {
        return 0;
    }
    if (addr.ss_family == AF_INET) {
        return ntohs(((struct sockaddr_in *)(void *)&addr)->sin_port);
    }
    if (addr.ss_family == AF_INET6) {
        return ntohs(((struct sockaddr_in6 *)(void *)&addr)->sin6_port);
    }
    return 0;
}

static void handle_close(PlumHandle *h) {
    if (h->fd >= 0) {
        close_socket((sock_t)h->fd);
        h->fd = -1;
    }
}

static PlumHandle *handle_new(const char *kind, sock_t s) {
    PlumHandle *h = plum_alloc_zeroed(1, sizeof *h);
    h->obj.refcount = 1;
    h->obj.type = PLUM_OBJ_HANDLE;
    h->kind = kind;
    h->fd = (intptr_t)s;
    h->close = handle_close;
    h->port = local_port(s);
    return h;
}

static bool open_handle(PlumNative *n, PlumValue v, const char *what, PlumHandle **out) {
    PlumHandle *h = plum_as_handle(v);
    if (h->fd < 0) {
        return plum_native_throw(n, PLUM_ERROR_NET, "%s: the %s is closed", what, h->kind);
    }
    *out = h;
    return true;
}

/* Look `host` up; `passive` for an address to listen on. NULL after throwing. */
static struct addrinfo *lookup(PlumNative *n, const char *host, int64_t port, int type,
                               bool passive) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = type;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    char service[16];
    snprintf(service, sizeof service, "%lld", (long long)port);
    struct addrinfo *found = NULL;
    int err = getaddrinfo(host[0] != '\0' ? host : NULL, service, &hints, &found);
    if (err != 0 || found == NULL) {
        plum_native_throw(n, PLUM_ERROR_NET, "can't find the address of '%s': %s", host,
                          gai_strerror(err));
        return NULL;
    }
    return found;
}

/* ------------------------------------------------------------------ */
/* net.listen, net.connect, net.udp, net.resolve                       */
/* ------------------------------------------------------------------ */

static bool n_listen(ARGS) {
    int64_t port = args[0].as.i;
    const char *host = argc > 1 && args[1].type != PLUM_VAL_NONE ? str_arg(args[1]) : "0.0.0.0";
    if (!check_port(n, port, true)) {
        return false;
    }
    net_start();
    struct addrinfo *ai = lookup(n, host, port, SOCK_STREAM, true);
    if (ai == NULL) {
        return false;
    }
    sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == BAD_SOCKET) {
        freeaddrinfo(ai);
        return net_fail(n, "listen", last_error());
    }
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
    if (bind(s, ai->ai_addr, (socklen_t)ai->ai_addrlen) != 0 || listen(s, 512) != 0 ||
        !set_nonblocking(s)) {
        int err = last_error();
        freeaddrinfo(ai);
        close_socket(s);
        char what[96];
        snprintf(what, sizeof what, "listen on %s port %lld", host, (long long)port);
        return net_fail(n, what, err);
    }
    freeaddrinfo(ai);
    PlumHandle *h = handle_new("Listener", s);
    *result = plum_obj(&h->obj);
    return true;
}

/* A connection under way, kept across the waits of net.connect. */
typedef struct Connecting {
    sock_t s;
    char peer[96];
} Connecting;

static void connecting_free(void *p) {
    Connecting *c = p;
    if (c->s != BAD_SOCKET) {
        close_socket(c->s);
    }
    plum_free(c);
}

static bool n_connect(ARGS) {
    const char *host = str_arg(args[0]);
    int64_t port = args[1].as.i;
    double timeout = timeout_arg(args, argc, 2);
    if (!check_port(n, port, false) || !check_timeout(n, timeout)) {
        return false;
    }
    void **state = plum_native_call_state(n, connecting_free);
    Connecting *c = *state;
    if (c == NULL) {
        /* the first time: start connecting */
        net_start();
        struct addrinfo *ai = lookup(n, host, port, SOCK_STREAM, false);
        if (ai == NULL) {
            return false;
        }
        c = plum_alloc_zeroed(1, sizeof *c);
        c->s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (c->s == BAD_SOCKET || !set_nonblocking(c->s)) {
            int err = last_error();
            freeaddrinfo(ai);
            connecting_free(c);
            return net_fail(n, "connect", err);
        }
        address_text(ai->ai_addr, c->peer, sizeof c->peer);
        *state = c;
        int r = connect(c->s, ai->ai_addr, (socklen_t)ai->ai_addrlen);
        freeaddrinfo(ai);
        if (r != 0 && !would_block(last_error())) {
            char what[160];
            snprintf(what, sizeof what, "connect to %s port %lld", host, (long long)port);
            return net_fail(n, what, last_error());
        }
    }
    for (;;) {
        /* writable means connected (or failed) */
        int ready = plum_native_wait_io(n, (intptr_t)c->s, true, timeout);
        if (ready == 0) {
            return false;
        }
        if (ready < 0) {
            return plum_native_throw(n, PLUM_ERROR_NET, "connect to %s port %lld: timed out", host,
                                     (long long)port);
        }
        int err = 0;
        socklen_t len = sizeof err;
        getsockopt(c->s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
        if (err != 0) {
            char what[160];
            snprintf(what, sizeof what, "connect to %s port %lld", host, (long long)port);
            return net_fail(n, what, err);
        }
        break;
    }
    PlumHandle *h = handle_new("Connection", c->s);
    snprintf(h->peer, sizeof h->peer, "%s", c->peer);
    c->s = BAD_SOCKET; /* the handle has it now */
    *result = plum_obj(&h->obj);
    return true;
}

static bool n_udp(ARGS) {
    int64_t port = argc > 0 && args[0].type == PLUM_VAL_INT ? args[0].as.i : 0;
    const char *host = argc > 1 && args[1].type != PLUM_VAL_NONE ? str_arg(args[1]) : "0.0.0.0";
    if (!check_port(n, port, true)) {
        return false;
    }
    net_start();
    struct addrinfo *ai = lookup(n, host, port, SOCK_DGRAM, true);
    if (ai == NULL) {
        return false;
    }
    sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == BAD_SOCKET || bind(s, ai->ai_addr, (socklen_t)ai->ai_addrlen) != 0 ||
        !set_nonblocking(s)) {
        int err = last_error();
        if (s != BAD_SOCKET) {
            close_socket(s);
        }
        freeaddrinfo(ai);
        return net_fail(n, "udp", err);
    }
    freeaddrinfo(ai);
    PlumHandle *h = handle_new("UdpSocket", s);
    *result = plum_obj(&h->obj);
    return true;
}

static bool n_resolve(ARGS) {
    (void)argc;
    const char *host = str_arg(args[0]);
    net_start();
    struct addrinfo *ai = lookup(n, host, 0, SOCK_STREAM, false);
    if (ai == NULL) {
        return false;
    }
    PlumList *out = plum_list_new(0);
    for (struct addrinfo *a = ai; a != NULL; a = a->ai_next) {
        char text[96];
        address_text(a->ai_addr, text, sizeof text);
        char *colon = strrchr(text, ':'); /* the address without ":0" */
        if (colon != NULL) {
            *colon = '\0';
        }
        size_t len = strlen(text);
        if (len > 1 && text[0] == '[' && text[len - 1] == ']') {
            memmove(text, text + 1, len - 2);
            text[len - 2] = '\0';
        }
        bool seen = false;
        for (size_t i = 0; i < out->len && !seen; i++) {
            seen = strcmp(plum_as_string(out->items[i])->chars, text) == 0;
        }
        if (!seen) {
            plum_list_push(out, plum_str_value(text, strlen(text)));
        }
    }
    freeaddrinfo(ai);
    *result = plum_obj(&out->obj);
    return true;
}

/* ------------------------------------------------------------------ */
/* Listener                                                            */
/* ------------------------------------------------------------------ */

static bool l_accept(ARGS) {
    PlumHandle *h = NULL;
    double timeout = timeout_arg(args, argc, 1);
    if (!open_handle(n, args[0], "accept", &h) || !check_timeout(n, timeout)) {
        return false;
    }
    for (;;) {
        struct sockaddr_storage addr;
        socklen_t len = sizeof addr;
        sock_t s = accept((sock_t)h->fd, (struct sockaddr *)&addr, &len);
        if (s != BAD_SOCKET) {
            if (!set_nonblocking(s)) {
                int err = last_error();
                close_socket(s);
                return net_fail(n, "accept", err);
            }
            PlumHandle *c = handle_new("Connection", s);
            address_text((struct sockaddr *)&addr, c->peer, sizeof c->peer);
            *result = plum_obj(&c->obj);
            return true;
        }
        int err = last_error();
        if (!would_block(err)) {
            return net_fail(n, "accept", err);
        }
        int ready = plum_native_wait_io(n, h->fd, false, timeout);
        if (ready == 0) {
            return false;
        }
        if (ready < 0) {
            return plum_native_throw(n, PLUM_ERROR_NET, "accept: timed out");
        }
    }
}

static bool h_port(ARGS) {
    (void)n;
    (void)argc;
    *result = plum_int(plum_as_handle(args[0])->port);
    return true;
}

static bool h_close(ARGS) {
    (void)n;
    (void)argc;
    handle_close(plum_as_handle(args[0]));
    *result = plum_none();
    return true;
}

/* ------------------------------------------------------------------ */
/* Connection                                                          */
/* ------------------------------------------------------------------ */

/*
 * Read what's there into the handle's buffer (at most `max` bytes): true
 * with *got = 0 at the end of the stream. Returns false after throwing or
 * giving way.
 */
static bool fill(PlumNative *n, PlumHandle *h, size_t max, double timeout, size_t *got) {
    if (h->in_len + max > h->in_cap) {
        size_t cap = h->in_cap < 4096 ? 4096 : h->in_cap;
        while (cap < h->in_len + max) {
            cap = plum_mul_size(cap, 2);
        }
        h->in = plum_realloc_array(h->in, cap, 1);
        h->in_cap = cap;
    }
    for (;;) {
        int r =
            (int)recv((sock_t)h->fd, h->in + h->in_len, (int)(max > 1 << 30 ? 1 << 30 : max), 0);
        if (r >= 0) {
            h->in_len += (size_t)r;
            *got = (size_t)r;
            return true;
        }
        int err = last_error();
        if (!would_block(err)) {
            return net_fail(n, "read", err);
        }
        int ready = plum_native_wait_io(n, h->fd, false, timeout);
        if (ready == 0) {
            return false;
        }
        if (ready < 0) {
            return plum_native_throw(n, PLUM_ERROR_NET, "read: timed out");
        }
    }
}

/* Take the first `len` buffered bytes as a string. */
static PlumValue take(PlumHandle *h, size_t len, size_t skip) {
    PlumValue v = plum_str_value(h->in, len);
    size_t used = len + skip;
    memmove(h->in, h->in + used, h->in_len - used);
    h->in_len -= used;
    return v;
}

/* read(max): what has arrived, up to max bytes; none at the end. */
static bool c_read(ARGS) {
    PlumHandle *h = NULL;
    int64_t max = argc > 1 && args[1].type == PLUM_VAL_INT ? args[1].as.i : 65536;
    double timeout = timeout_arg(args, argc, 2);
    if (!open_handle(n, args[0], "read", &h) || !check_timeout(n, timeout)) {
        return false;
    }
    if (max < 1 || max > 16 * 1024 * 1024) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "read: max must be 1 to 16 MB, not %lld",
                                 (long long)max);
    }
    if (h->in_len == 0) {
        size_t got;
        if (!fill(n, h, (size_t)max, timeout, &got)) {
            return false;
        }
        if (got == 0) {
            *result = plum_none(); /* the other end closed */
            return true;
        }
    }
    *result = take(h, h->in_len < (size_t)max ? h->in_len : (size_t)max, 0);
    return true;
}

/* read_line(): a line without its \n (or \r\n); none at the end. */
static bool c_read_line(ARGS) {
    PlumHandle *h = NULL;
    double timeout = timeout_arg(args, argc, 1);
    if (!open_handle(n, args[0], "read_line", &h) || !check_timeout(n, timeout)) {
        return false;
    }
    for (;;) {
        char *nl = h->in_len > 0 ? memchr(h->in, '\n', h->in_len) : NULL;
        if (nl != NULL) {
            size_t len = (size_t)(nl - h->in);
            bool cr = len > 0 && h->in[len - 1] == '\r';
            *result = take(h, cr ? len - 1 : len, cr ? 2 : 1);
            return true;
        }
        if (h->in_len > 1024 * 1024) {
            return plum_native_throw(n, PLUM_ERROR_NET, "read_line: a line longer than 1 MB");
        }
        size_t got;
        if (!fill(n, h, 4096, timeout, &got)) {
            return false;
        }
        if (got == 0) {
            /* the end: what's left is the last line, if anything */
            *result = h->in_len > 0 ? take(h, h->in_len, 0) : plum_none();
            return true;
        }
    }
}

typedef struct Sending {
    size_t sent;
} Sending;

static void sending_free(void *p) {
    plum_free(p);
}

/* write(text): all of it, waiting for room as needed. */
static bool c_write(ARGS) {
    PlumHandle *h = NULL;
    double timeout = timeout_arg(args, argc, 2);
    if (!open_handle(n, args[0], "write", &h) || !check_timeout(n, timeout)) {
        return false;
    }
    const PlumString *text = plum_as_string(args[1]);
    void **state = plum_native_call_state(n, sending_free);
    if (*state == NULL) {
        *state = plum_alloc_zeroed(1, sizeof(Sending));
    }
    Sending *s = *state;
    while (s->sent < text->len) {
        size_t left = text->len - s->sent;
        int r = (int)send((sock_t)h->fd, text->chars + s->sent,
                          (int)(left > 1 << 30 ? 1 << 30 : left), SEND_FLAGS);
        if (r >= 0) {
            s->sent += (size_t)r;
            continue;
        }
        int err = last_error();
        if (!would_block(err)) {
            return net_fail(n, "write", err);
        }
        int ready = plum_native_wait_io(n, h->fd, true, timeout);
        if (ready == 0) {
            return false; /* made again later, from s->sent on */
        }
        if (ready < 0) {
            return plum_native_throw(n, PLUM_ERROR_NET, "write: timed out");
        }
    }
    *result = plum_none();
    return true;
}

static bool c_remote(ARGS) {
    (void)n;
    (void)argc;
    const char *peer = plum_as_handle(args[0])->peer;
    *result = plum_str_value(peer, strlen(peer));
    return true;
}

/* ------------------------------------------------------------------ */
/* UdpSocket                                                           */
/* ------------------------------------------------------------------ */

static bool u_send_to(ARGS) {
    (void)argc;
    PlumHandle *h = NULL;
    if (!open_handle(n, args[0], "send_to", &h) || !check_port(n, args[2].as.i, false)) {
        return false;
    }
    const PlumString *text = plum_as_string(args[3]);
    if (text->len > 65507) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "send_to: a datagram holds at most 65,507 bytes, not %zu",
                                 text->len);
    }
    struct addrinfo *ai = lookup(n, str_arg(args[1]), args[2].as.i, SOCK_DGRAM, false);
    if (ai == NULL) {
        return false;
    }
    for (;;) {
        int r = (int)sendto((sock_t)h->fd, text->chars, (int)text->len, 0, ai->ai_addr,
                            (socklen_t)ai->ai_addrlen);
        if (r >= 0) {
            break;
        }
        int err = last_error();
        if (!would_block(err)) {
            freeaddrinfo(ai);
            return net_fail(n, "send_to", err);
        }
        int ready = plum_native_wait_io(n, h->fd, true, 0);
        if (ready == 0) {
            freeaddrinfo(ai);
            return false;
        }
    }
    freeaddrinfo(ai);
    *result = plum_none();
    return true;
}

/* receive(): the next datagram; sender() says where it came from. */
static bool u_receive(ARGS) {
    PlumHandle *h = NULL;
    int64_t max = argc > 1 && args[1].type == PLUM_VAL_INT ? args[1].as.i : 65536;
    double timeout = timeout_arg(args, argc, 2);
    if (!open_handle(n, args[0], "receive", &h) || !check_timeout(n, timeout)) {
        return false;
    }
    if (max < 1 || max > 65536) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "receive: max must be 1 to 65,536");
    }
    char *buf = plum_alloc((size_t)max);
    for (;;) {
        struct sockaddr_storage from;
        socklen_t len = sizeof from;
        int r = (int)recvfrom((sock_t)h->fd, buf, (int)max, 0, (struct sockaddr *)&from, &len);
        if (r < 0 && truncated(last_error())) {
            r = (int)max; /* the first max bytes, as everywhere else */
        }
        if (r >= 0) {
            address_text((struct sockaddr *)&from, h->peer, sizeof h->peer);
            *result = plum_str_value(buf, (size_t)r);
            plum_free(buf);
            return true;
        }
        int err = last_error();
        if (!would_block(err)) {
            plum_free(buf);
            return net_fail(n, "receive", err);
        }
        int ready = plum_native_wait_io(n, h->fd, false, timeout);
        if (ready <= 0) {
            plum_free(buf);
            return ready == 0 ? false : plum_native_throw(n, PLUM_ERROR_NET, "receive: timed out");
        }
    }
}

/* ------------------------------------------------------------------ */
/* The tables                                                          */
/* ------------------------------------------------------------------ */

static const PlumNativeFunction net_functions[] = {
    {"listen", "(port: int, host: str = \"0.0.0.0\") -> Listener", n_listen,
     "a TCP listener on a port (0 picks a free one); listener.accept() takes connections"},
    {"connect", "(host: str, port: int, timeout: float = 0) -> Connection", n_connect,
     "a TCP connection to host and port"},
    {"udp", "(port: int = 0, host: str = \"0.0.0.0\") -> UdpSocket", n_udp,
     "a UDP socket on a port (0 picks a free one)"},
    {"resolve", "(host: str) -> list[str]", n_resolve, "the addresses a host name has"},
};

const PlumNativeModule plum_module_net = {
    "net",         "TCP and UDP sockets, and DNS",
    net_functions, sizeof net_functions / sizeof net_functions[0],
    NULL,          0,
};

static const PlumNativeFunction listener_methods[] = {
    {"accept", "(self: Listener, timeout: float = 0) -> Connection", l_accept,
     "the next connection, waiting for one"},
    {"port", "(self: Listener) -> int", h_port, "the port it listens on"},
    {"close", "(self: Listener)", h_close, "stop listening"},
};

const PlumNativeModule plum_module_listener = {
    "net.listener",
    "the methods of Listener (no seed needed)",
    listener_methods,
    sizeof listener_methods / sizeof listener_methods[0],
    NULL,
    0,
};

static const PlumNativeFunction connection_methods[] = {
    {"read", "(self: Connection, max: int = 65536, timeout: float = 0) -> str?", c_read,
     "what has arrived (waiting for something), up to max bytes; none once the other end closes"},
    {"read_line", "(self: Connection, timeout: float = 0) -> str?", c_read_line,
     "the next line, without its line break; none at the end"},
    {"write", "(self: Connection, text: str, timeout: float = 0)", c_write,
     "send all of text, waiting for room as needed"},
    {"remote", "(self: Connection) -> str", c_remote, "the other end, as \"host:port\""},
    {"port", "(self: Connection) -> int", h_port, "this end's port"},
    {"close", "(self: Connection)", h_close, "close the connection"},
};

const PlumNativeModule plum_module_connection = {
    "net.connection",
    "the methods of Connection (no seed needed)",
    connection_methods,
    sizeof connection_methods / sizeof connection_methods[0],
    NULL,
    0,
};

static const PlumNativeFunction udp_methods[] = {
    {"send_to", "(self: UdpSocket, host: str, port: int, text: str)", u_send_to,
     "send one datagram"},
    {"receive", "(self: UdpSocket, max: int = 65536, timeout: float = 0) -> str", u_receive,
     "the next datagram, waiting for one"},
    {"sender", "(self: UdpSocket) -> str", c_remote,
     "where the last datagram received came from, as \"host:port\""},
    {"port", "(self: UdpSocket) -> int", h_port, "the port it is on"},
    {"close", "(self: UdpSocket)", h_close, "close the socket"},
};

const PlumNativeModule plum_module_udp = {
    "net.udp",   "the methods of UdpSocket (no seed needed)",
    udp_methods, sizeof udp_methods / sizeof udp_methods[0],
    NULL,        0,
};
