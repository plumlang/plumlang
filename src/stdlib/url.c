/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The url module: parsing, building and resolving URLs, percent-encoding,
 * and query strings.
 *
 * Parsing follows the WHATWG URL Standard (https://url.spec.whatwg.org)
 * for what URLs commonly hold, as browsers do:
 *
 *   - tabs and line breaks are removed, and spaces and control characters
 *     trimmed from the ends;
 *   - the scheme and, for the special schemes (http, https, ws, wss, ftp,
 *     file), the host are lowercased; a special scheme's default port is
 *     dropped; `\` counts as `/` in them, and their path is at least "/";
 *   - IPv6 hosts are written in their shortest form, and IPv4 hosts in
 *     any of the forms browsers take (127.1, 0x7f.0.0.1) as four numbers;
 *   - "." and ".." path segments (also written %2e) are resolved;
 *   - characters a part can't hold as they are are percent-encoded, each
 *     with the set the standard gives that part; existing %XX stay.
 *
 * Where this differs from the standard (docs/stdlib/url.md): no IDNA, so
 * a host that isn't ASCII stays UTF-8 rather than becoming punycode; no
 * Windows drive-letter rules for file: URLs; and a URL with a special
 * scheme must name it ("http:example.com" isn't read as relative).
 */
#include "stdlib/native.h"

#include "util/array.h"
#include "vm/table.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ARGS PlumNative *n, const PlumValue *args, int argc, PlumValue *result

typedef PLUM_ARRAY(char) Buf;

static void put(Buf *b, const char *s, size_t len) {
    if (len == 0) {
        return;
    }
    plum_array_reserve(b, len);
    memcpy(b->items + b->len, s, len);
    b->len += len;
}

static void put_str(Buf *b, const char *s) {
    put(b, s, strlen(s));
}

static void put_char(Buf *b, char c) {
    plum_array_push(b, c);
}

static PlumValue buf_value(const Buf *b) {
    return plum_str_value(b->items != NULL ? b->items : "", b->len);
}

static bool buf_is(const Buf *b, const char *s) {
    size_t len = strlen(s);
    return b->len == len && (len == 0 || memcmp(b->items, s, len) == 0);
}

/* ------------------------------------------------------------------ */
/* Percent-encoding                                                    */
/* ------------------------------------------------------------------ */

/* The WHATWG percent-encode sets, as tests on a byte. */
typedef enum EncodeSet {
    SET_C0,            /* C0 controls and everything past '~' */
    SET_FRAGMENT,      /* C0, space " < > ` */
    SET_QUERY,         /* C0, space " # < > */
    SET_SPECIAL_QUERY, /* the query set and ' */
    SET_PATH,          /* the query set and ? ` { } */
    SET_USERINFO,      /* the path set and / : ; = @ [ \ ] ^ | */
    SET_COMPONENT,     /* all but A-Z a-z 0-9 - . _ ~ (RFC 3986 unreserved) */
    SET_FORM,          /* all but A-Z a-z 0-9 * - . _ (space is written +) */
} EncodeSet;

static bool is_alnum(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static bool in_set(unsigned char c, EncodeSet set) {
    if (set == SET_COMPONENT) {
        return !(is_alnum(c) || c == '-' || c == '.' || c == '_' || c == '~');
    }
    if (set == SET_FORM) {
        return !(is_alnum(c) || c == '*' || c == '-' || c == '.' || c == '_');
    }
    if (c < 0x20 || c > 0x7E) {
        return true;
    }
    switch (set) {
    case SET_C0:
        return false;
    case SET_FRAGMENT:
        return c == ' ' || c == '"' || c == '<' || c == '>' || c == '`';
    case SET_QUERY:
        return c == ' ' || c == '"' || c == '#' || c == '<' || c == '>';
    case SET_SPECIAL_QUERY:
        return in_set(c, SET_QUERY) || c == '\'';
    case SET_PATH:
        return in_set(c, SET_QUERY) || c == '?' || c == '`' || c == '{' || c == '}';
    case SET_USERINFO:
        return in_set(c, SET_PATH) || c == '/' || c == ':' || c == ';' || c == '=' || c == '@' ||
               (c >= '[' && c <= '^') || c == '|';
    default:
        return false;
    }
}

static void encode_into(Buf *b, const char *s, size_t len, EncodeSet set) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (set == SET_FORM && c == ' ') {
            put_char(b, '+');
        } else if (in_set(c, set)) {
            char esc[3] = {'%', hex[c >> 4], hex[c & 0x0F]};
            put(b, esc, 3);
        } else {
            put_char(b, (char)c);
        }
    }
}

static int hex_value(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

/* %XX to its byte; a % without two hex digits stays as it is. With
 * `plus`, + is a space (form encoding). */
static void decode_into(Buf *b, const char *s, size_t len, bool plus) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '%' && i + 2 < len && hex_value(s[i + 1]) >= 0 && hex_value(s[i + 2]) >= 0) {
            put_char(b, (char)(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2])));
            i += 2;
        } else if (plus && s[i] == '+') {
            put_char(b, ' ');
        } else {
            put_char(b, s[i]);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Parsed URLs                                                         */
/* ------------------------------------------------------------------ */

typedef struct Url {
    Buf scheme, username, password, host, path, query, fragment;
    long port;      /* -1: none (or the scheme's default) */
    bool has_host;  /* there is an authority (its host may be empty) */
    bool has_query; /* a '?', perhaps with nothing after it */
    bool has_fragment;
    bool opaque; /* the path isn't a list of segments: mailto:x@example.com */
    char error[200];
} Url;

static void url_free(Url *u) {
    plum_array_free(&u->scheme);
    plum_array_free(&u->username);
    plum_array_free(&u->password);
    plum_array_free(&u->host);
    plum_array_free(&u->path);
    plum_array_free(&u->query);
    plum_array_free(&u->fragment);
}

PLUM_PRINTF(2, 3)
static bool url_fail(Url *u, const char *fmt, ...);

static bool url_fail(Url *u, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(u->error, sizeof u->error, fmt, args);
    va_end(args);
    return false;
}

static long default_port(const Buf *scheme) {
    return buf_is(scheme, "http") || buf_is(scheme, "ws")     ? 80
           : buf_is(scheme, "https") || buf_is(scheme, "wss") ? 443
           : buf_is(scheme, "ftp")                            ? 21
                                                              : -1;
}

static bool is_special(const Buf *scheme) {
    return default_port(scheme) >= 0 || buf_is(scheme, "file");
}

/* "/" always, and "\" too in a special URL. */
static bool is_slash(char c, bool special) {
    return c == '/' || (special && c == '\\');
}

/* ---- Hosts --------------------------------------------------------- */

/* An IPv6 address in [brackets], without them, into `pieces`. */
static bool parse_ipv6(const char *s, size_t len, uint16_t pieces[8]) {
    memset(pieces, 0, 8 * sizeof pieces[0]);
    int at = 0, compress = -1;
    size_t i = 0;
    if (len >= 1 && s[0] == ':') {
        if (len < 2 || s[1] != ':') {
            return false;
        }
        i = 2;
        compress = at = 0;
        if (i == len) {
            return true; /* "::" */
        }
    }
    while (i < len) {
        if (at == 8) {
            return false;
        }
        if (s[i] == ':') {
            if (compress >= 0) {
                return false;
            }
            i++;
            compress = at;
            if (i == len) {
                break;
            }
            continue;
        }
        unsigned value = 0;
        size_t digits = 0;
        while (digits < 4 && i < len && hex_value(s[i]) >= 0) {
            value = value * 16 + (unsigned)hex_value(s[i]);
            i++;
            digits++;
        }
        if (i < len && s[i] == '.') {
            /* an IPv4 address in the last two pieces */
            if (digits == 0 || at > 6) {
                return false;
            }
            i -= digits;
            int numbers = 0;
            while (i < len) {
                int part = -1;
                if (numbers > 0) {
                    if (s[i] != '.' || numbers == 4) {
                        return false;
                    }
                    i++;
                }
                if (i >= len || s[i] < '0' || s[i] > '9') {
                    return false;
                }
                while (i < len && s[i] >= '0' && s[i] <= '9') {
                    int d = s[i] - '0';
                    if (part == 0) {
                        return false; /* a leading zero */
                    }
                    part = part < 0 ? d : part * 10 + d;
                    if (part > 255) {
                        return false;
                    }
                    i++;
                }
                pieces[at] = (uint16_t)(pieces[at] * 0x100 + part);
                numbers++;
                if (numbers == 2 || numbers == 4) {
                    at++;
                }
            }
            if (numbers != 4) {
                return false;
            }
            break;
        }
        if (digits == 0) {
            return false;
        }
        if (i < len) {
            if (s[i] != ':') {
                return false;
            }
            i++;
            if (i == len) {
                return false;
            }
        }
        pieces[at++] = (uint16_t)value;
    }
    if (compress >= 0) {
        int swaps = at - compress;
        at = 7;
        while (at != 0 && swaps > 0) {
            uint16_t t = pieces[at];
            pieces[at] = pieces[compress + swaps - 1];
            pieces[compress + swaps - 1] = t;
            at--;
            swaps--;
        }
    } else if (at != 8) {
        return false;
    }
    return true;
}

/* The shortest form: the longest run of two or more zero pieces as "::". */
static void put_ipv6(Buf *b, const uint16_t pieces[8]) {
    int best = -1, best_len = 1;
    for (int i = 0; i < 8;) {
        int j = i;
        while (j < 8 && pieces[j] == 0) {
            j++;
        }
        if (j - i > best_len) {
            best = i;
            best_len = j - i;
        }
        i = j > i ? j : i + 1;
    }
    put_char(b, '[');
    bool ignore0 = false;
    for (int i = 0; i < 8; i++) {
        if (ignore0 && pieces[i] == 0) {
            continue;
        }
        ignore0 = false;
        if (i == best) {
            put_str(b, i == 0 ? "::" : ":");
            ignore0 = true;
            continue;
        }
        char text[8];
        int w = snprintf(text, sizeof text, "%x", (unsigned)pieces[i]);
        put(b, text, (size_t)w);
        if (i != 7) {
            put_char(b, ':');
        }
    }
    put_char(b, ']');
}

/* One part of an IPv4 address: decimal, 0x hex or 0 octal. -1 if it isn't
 * a number; the value is capped just past what any part can be. */
static long long ipv4_number(const char *s, size_t len) {
    int base = 10;
    if (len >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
        len -= 2;
    } else if (len >= 2 && s[0] == '0') {
        base = 8;
        s++;
        len--;
    }
    if (len == 0) {
        return base == 10 ? -1 : 0;
    }
    long long v = 0;
    for (size_t i = 0; i < len; i++) {
        int d = hex_value(s[i]);
        if (d < 0 || d >= base) {
            return -1;
        }
        v = v * base + d;
        if (v > 0x1FFFFFFFFLL) {
            v = 0x1FFFFFFFFLL;
        }
    }
    return v;
}

/* Does the host's last label (a trailing dot aside) look like a number,
 * making the host an IPv4 address? */
static bool ends_in_number(const char *s, size_t len) {
    if (len > 0 && s[len - 1] == '.') {
        len--;
    }
    size_t start = len;
    while (start > 0 && s[start - 1] != '.') {
        start--;
    }
    const char *last = s + start;
    size_t n = len - start;
    if (n == 0) {
        return false;
    }
    bool digits = true;
    for (size_t i = 0; i < n; i++) {
        digits = digits && last[i] >= '0' && last[i] <= '9';
    }
    return digits || ipv4_number(last, n) >= 0;
}

static bool parse_ipv4(const char *s, size_t len, uint32_t *out) {
    if (len > 0 && s[len - 1] == '.') {
        len--;
    }
    long long parts[4];
    int count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || s[i] == '.') {
            if (count == 4) {
                return false;
            }
            long long v = ipv4_number(s + start, i - start);
            if (v < 0) {
                return false;
            }
            parts[count++] = v;
            start = i + 1;
        }
    }
    for (int i = 0; i < count - 1; i++) {
        if (parts[i] > 255) {
            return false;
        }
    }
    long long last = parts[count - 1];
    if (last >= (1LL << (8 * (5 - count)))) {
        return false;
    }
    uint32_t v = (uint32_t)last;
    for (int i = 0; i < count - 1; i++) {
        v += (uint32_t)parts[i] << (8 * (3 - i));
    }
    *out = v;
    return true;
}

/* Characters no host may hold; a domain (a special URL's host) also no
 * control characters or %. */
static bool forbidden_host(unsigned char c, bool opaque) {
    return (opaque ? c == 0 || c == '\t' || c == '\n' || c == '\r' || c == ' ' : c <= 0x20) ||
           c == '#' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' || c == '@' ||
           c == '[' || c == '\\' || c == ']' || c == '^' || c == '|' ||
           (!opaque && (c == '%' || c == 0x7F));
}

static bool parse_host(Url *u, const char *s, size_t len, bool special) {
    if (len > 0 && s[0] == '[') {
        uint16_t pieces[8];
        if (len < 2 || s[len - 1] != ']' || !parse_ipv6(s + 1, len - 2, pieces)) {
            return url_fail(u, "\"%.*s\" isn't an IPv6 address", (int)(len > 60 ? 60 : len), s);
        }
        put_ipv6(&u->host, pieces);
        return true;
    }
    if (!special) {
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)s[i];
            if (forbidden_host(c, true)) {
                if (c <= 0x20) {
                    return url_fail(u, "a host can't hold the character 0x%02X", c);
                }
                return url_fail(u, "a host can't hold '%c'", (char)c);
            }
        }
        encode_into(&u->host, s, len, SET_C0);
        return true;
    }
    Buf domain = {0};
    decode_into(&domain, s, len, false);
    for (size_t i = 0; i < domain.len; i++) {
        unsigned char c = (unsigned char)domain.items[i];
        if (c >= 'A' && c <= 'Z') {
            domain.items[i] = (char)(c + 32);
        } else if (c < 0x80 && forbidden_host(c, false)) {
            plum_array_free(&domain);
            if (c < 0x20 || c == 0x7F) {
                return url_fail(u, "a host can't hold the character 0x%02X", c);
            }
            return url_fail(u, "a host can't hold '%c'", (char)c);
        }
    }
    if (domain.len > 0 && ends_in_number(domain.items, domain.len)) {
        uint32_t ip;
        if (!parse_ipv4(domain.items, domain.len, &ip)) {
            plum_array_free(&domain);
            return url_fail(u, "the host ends in a number but isn't an IPv4 address");
        }
        char text[16];
        int w = snprintf(text, sizeof text, "%u.%u.%u.%u", (unsigned)(ip >> 24),
                         (unsigned)(ip >> 16 & 0xFF), (unsigned)(ip >> 8 & 0xFF),
                         (unsigned)(ip & 0xFF));
        put(&u->host, text, (size_t)w);
    } else {
        put(&u->host, domain.items, domain.len);
    }
    plum_array_free(&domain);
    return true;
}

/* user:password@host:port */
static bool parse_authority(Url *u, const char *s, size_t len, bool special) {
    size_t at = len;
    for (size_t i = len; i > 0; i--) {
        if (s[i - 1] == '@') {
            at = i - 1;
            break;
        }
    }
    if (at < len) {
        size_t colon = at;
        for (size_t i = 0; i < at; i++) {
            if (s[i] == ':') {
                colon = i;
                break;
            }
        }
        encode_into(&u->username, s, colon, SET_USERINFO);
        if (colon < at) {
            encode_into(&u->password, s + colon + 1, at - colon - 1, SET_USERINFO);
        }
        s += at + 1;
        len -= at + 1;
    }
    /* the port's ':' is the last one outside brackets */
    size_t host_end = len;
    bool bracket = false;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '[') {
            bracket = true;
        } else if (s[i] == ']') {
            bracket = false;
        } else if (s[i] == ':' && !bracket) {
            host_end = i;
            break;
        }
    }
    if (host_end == 0 && ((special && !buf_is(&u->scheme, "file")) || at < len || host_end < len)) {
        return url_fail(u, "it has no host; write one after %s://", u->scheme.items);
    }
    if (!parse_host(u, s, host_end, special)) {
        return false;
    }
    if (host_end < len) {
        const char *p = s + host_end + 1;
        size_t plen = len - host_end - 1;
        long port = 0;
        for (size_t i = 0; i < plen; i++) {
            if (p[i] < '0' || p[i] > '9') {
                return url_fail(u, "the port must be a number, not \"%.*s\"",
                                (int)(plen > 20 ? 20 : plen), p);
            }
            port = port * 10 + (p[i] - '0');
            if (port > 65535) {
                return url_fail(u, "the port is past 65535");
            }
        }
        if (plen > 0) {
            u->port = port == default_port(&u->scheme) ? -1 : port;
        }
    }
    if (buf_is(&u->scheme, "file")) {
        if (u->username.len > 0 || u->password.len > 0 || u->port >= 0) {
            return url_fail(u, "a file: URL can't have a user name, password or port");
        }
        if (buf_is(&u->host, "localhost")) {
            u->host.len = 0;
        }
    }
    return true;
}

/* ---- Paths --------------------------------------------------------- */

static bool segment_is(const char *s, size_t len, int dots) {
    size_t i = 0;
    int seen = 0;
    while (i < len) {
        if (s[i] == '.') {
            i++;
        } else if (len - i >= 3 && s[i] == '%' && s[i + 1] == '2' &&
                   (s[i + 2] == 'e' || s[i + 2] == 'E')) {
            i += 3;
        } else {
            return false;
        }
        seen++;
    }
    return seen == dots;
}

/*
 * A path made of segments: `s` is what follows the authority (or the
 * scheme), starting with a slash or empty. "." and ".." are resolved.
 */
static void parse_path(Url *u, const char *s, size_t len, bool special) {
    /* the segments go into u->path as "/a/b"; ends[] says where each starts */
    PLUM_ARRAY(size_t) starts = {0};
    size_t i = 0;
    if (len > 0 && is_slash(s[0], special)) {
        i = 1;
    } else if (len == 0 && !special) {
        return;
    }
    for (;;) {
        size_t start = i;
        while (i < len && !is_slash(s[i], special)) {
            i++;
        }
        bool last = i >= len;
        const char *seg = s + start;
        size_t seg_len = i - start;
        if (segment_is(seg, seg_len, 2)) {
            if (starts.len > 0) {
                u->path.len = plum_array_pop(&starts);
            }
            if (last) {
                plum_array_push(&starts, u->path.len);
                put_char(&u->path, '/');
            }
        } else if (segment_is(seg, seg_len, 1)) {
            if (last) {
                plum_array_push(&starts, u->path.len);
                put_char(&u->path, '/');
            }
        } else {
            plum_array_push(&starts, u->path.len);
            put_char(&u->path, '/');
            encode_into(&u->path, seg, seg_len, SET_PATH);
        }
        if (last) {
            break;
        }
        i++;
    }
    plum_array_free(&starts);
}

/* ---- The whole URL ------------------------------------------------- */

static bool valid_scheme(const char *s, size_t len, size_t *colon) {
    if (len == 0 || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'))) {
        return false;
    }
    for (size_t i = 1; i < len; i++) {
        char c = s[i];
        if (c == ':') {
            *colon = i;
            return true;
        }
        if (!is_alnum((unsigned char)c) && c != '+' && c != '-' && c != '.') {
            return false;
        }
    }
    return false;
}

/* The text with spaces and controls trimmed from the ends and every tab
 * and line break removed, as the standard does first. */
static void clean_input(Buf *out, const char *s, size_t len) {
    while (len > 0 && (unsigned char)s[0] <= 0x20) {
        s++;
        len--;
    }
    while (len > 0 && (unsigned char)s[len - 1] <= 0x20) {
        len--;
    }
    for (size_t i = 0; i < len; i++) {
        if (s[i] != '\t' && s[i] != '\n' && s[i] != '\r') {
            put_char(out, s[i]);
        }
    }
}

static bool parse_url(Url *u, const char *text, size_t text_len) {
    memset(u, 0, sizeof *u);
    u->port = -1;
    Buf in = {0};
    clean_input(&in, text, text_len);
    const char *s = in.items != NULL ? in.items : "";
    size_t len = in.len;
    size_t colon = 0;
    if (!valid_scheme(s, len, &colon)) {
        plum_array_free(&in);
        return url_fail(u, "it has no scheme, like https:, at the start; for a URL relative to "
                           "another, use url.join(base, ref)");
    }
    for (size_t i = 0; i < colon; i++) {
        char c = s[i];
        put_char(&u->scheme, c >= 'A' && c <= 'Z' ? (char)(c + 32) : c);
    }
    put_char(&u->scheme, '\0');
    u->scheme.len--; /* NUL-terminated for messages */
    bool special = is_special(&u->scheme);
    bool file = buf_is(&u->scheme, "file");
    size_t i = colon + 1;

    /* where the path, query and fragment end */
    size_t q = len, h = len;
    for (size_t k = i; k < len; k++) {
        if (s[k] == '#') {
            h = k;
            break;
        }
        if (s[k] == '?' && q == len) {
            q = k;
        }
    }
    if (q > h) {
        q = h;
    }

    bool ok = true;
    if (special && !file) {
        while (i < q && is_slash(s[i], true)) {
            i++;
        }
        size_t end = i;
        while (end < q && !is_slash(s[end], true)) {
            end++;
        }
        u->has_host = true;
        ok = parse_authority(u, s + i, end - i, true);
        i = end;
    } else if (q - i >= 2 && is_slash(s[i], special) && is_slash(s[i + 1], special)) {
        i += 2;
        size_t end = i;
        while (end < q && !is_slash(s[end], special)) {
            end++;
        }
        u->has_host = true;
        ok = parse_authority(u, s + i, end - i, special);
        i = end;
    } else if (file) {
        u->has_host = true; /* file:/x is file:///x */
    }
    if (ok) {
        if (special || (i < q && s[i] == '/') || u->has_host) {
            parse_path(u, s + i, q - i, special);
        } else {
            u->opaque = true;
            encode_into(&u->path, s + i, q - i, SET_C0);
        }
        if (q < h) {
            u->has_query = true;
            encode_into(&u->query, s + q + 1, h - q - 1, special ? SET_SPECIAL_QUERY : SET_QUERY);
        }
        if (h < len) {
            u->has_fragment = true;
            encode_into(&u->fragment, s + h + 1, len - h - 1, SET_FRAGMENT);
        }
    }
    plum_array_free(&in);
    return ok;
}

static void put_authority(Buf *b, const Url *u) {
    put_str(b, "//");
    if (u->username.len > 0 || u->password.len > 0) {
        put(b, u->username.items, u->username.len);
        if (u->password.len > 0) {
            put_char(b, ':');
            put(b, u->password.items, u->password.len);
        }
        put_char(b, '@');
    }
    put(b, u->host.items, u->host.len);
    if (u->port >= 0) {
        char text[8];
        int w = snprintf(text, sizeof text, ":%ld", u->port);
        put(b, text, (size_t)w);
    }
}

static void put_href(Buf *b, const Url *u) {
    put(b, u->scheme.items, u->scheme.len);
    put_char(b, ':');
    if (u->has_host) {
        put_authority(b, u);
    } else if (!u->opaque && u->path.len >= 2 && u->path.items[0] == '/' &&
               u->path.items[1] == '/') {
        put_str(b, "/."); /* so the path isn't read back as an authority */
    }
    put(b, u->path.items, u->path.len);
    if (u->has_query) {
        put_char(b, '?');
        put(b, u->query.items, u->query.len);
    }
    if (u->has_fragment) {
        put_char(b, '#');
        put(b, u->fragment.items, u->fragment.len);
    }
}

static bool throw_url(PlumNative *n, const char *fn, const char *text, size_t len, const Url *u) {
    bool shown = len <= 80 && memchr(text, '\0', len) == NULL;
    return plum_native_throw(n, PLUM_ERROR_VALUE, "%s: \"%.*s%s\" isn't a URL: %s", fn,
                             shown ? (int)len : 77, text, shown ? "" : "...", u->error);
}

/* ------------------------------------------------------------------ */
/* The functions                                                       */
/* ------------------------------------------------------------------ */

static void map_put(PlumTable *map, const char *key, PlumValue value) {
    plum_table_put(map, plum_str_value(key, strlen(key)), value);
}

/* parse(text): the parts as a map of str. */
static bool url_parse(ARGS) {
    (void)argc;
    const PlumString *s = plum_as_string(args[0]);
    Url u;
    if (!parse_url(&u, s->chars, s->len)) {
        url_free(&u);
        return throw_url(n, "url.parse", s->chars, s->len, &u);
    }
    PlumTable *map = plum_native_new_map(n);
    map_put(map, "scheme", buf_value(&u.scheme));
    map_put(map, "username", buf_value(&u.username));
    map_put(map, "password", buf_value(&u.password));
    map_put(map, "host", buf_value(&u.host));
    char port[8] = "";
    int w = u.port >= 0 ? snprintf(port, sizeof port, "%ld", u.port) : 0;
    map_put(map, "port", plum_str_value(port, (size_t)w));
    map_put(map, "path", buf_value(&u.path));
    map_put(map, "query", buf_value(&u.query));
    map_put(map, "fragment", buf_value(&u.fragment));
    Buf href = {0};
    put_href(&href, &u);
    map_put(map, "href", buf_value(&href));
    plum_array_free(&href);
    url_free(&u);
    *result = plum_obj(&map->obj);
    return true;
}

/* normalize(text): the URL as parse reads it, written back. */
static bool url_normalize(ARGS) {
    (void)argc;
    const PlumString *s = plum_as_string(args[0]);
    Url u;
    if (!parse_url(&u, s->chars, s->len)) {
        url_free(&u);
        return throw_url(n, "url.normalize", s->chars, s->len, &u);
    }
    Buf href = {0};
    put_href(&href, &u);
    *result = buf_value(&href);
    plum_array_free(&href);
    url_free(&u);
    return true;
}

static const char *const part_names[] = {"scheme", "username", "password", "host", "port",
                                         "path",   "query",    "fragment", "href"};

/* The part `key` of the map, or NULL if it isn't there. */
static const PlumString *part(const PlumTable *map, const char *key) {
    PlumValue k = plum_str_value(key, strlen(key));
    const PlumValue *v = plum_table_find(map, k);
    plum_release(k);
    return v != NULL ? plum_as_string(*v) : NULL;
}

/* build(parts): a URL from its parts, each encoded as it needs. */
static bool url_build(ARGS) {
    (void)argc;
    const char *fn = "url.build";
    const PlumTable *map = plum_as_table(args[0]);
    int64_t pos = 0;
    const PlumTableEntry *e;
    while ((e = plum_table_next(map, &pos)) != NULL) {
        const PlumString *key = plum_as_string(e->key);
        bool known = false;
        for (size_t i = 0; i < sizeof part_names / sizeof part_names[0]; i++) {
            known = known || (key->len == strlen(part_names[i]) &&
                              memcmp(key->chars, part_names[i], key->len) == 0);
        }
        if (!known) {
            return plum_native_throw(n, PLUM_ERROR_VALUE,
                                     "%s: \"%.*s\" isn't a part of a URL; the parts are scheme, "
                                     "username, password, host, port, path, query and fragment",
                                     fn, key->len > 40 ? 40 : (int)key->len, key->chars);
        }
    }
    const PlumString *scheme = part(map, "scheme");
    if (scheme == NULL || scheme->len == 0) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "%s: the parts need a scheme, like "
                                 "{\"scheme\": \"https\", ...}",
                                 fn);
    }
    const PlumString *username = part(map, "username"), *password = part(map, "password"),
                     *host = part(map, "host"), *port = part(map, "port"),
                     *path = part(map, "path"), *query = part(map, "query"),
                     *fragment = part(map, "fragment");
    Buf b = {0};
    put(&b, scheme->chars, scheme->len);
    put_char(&b, ':');
    bool authority = host != NULL || (username != NULL && username->len > 0) ||
                     (password != NULL && password->len > 0) || (port != NULL && port->len > 0);
    if (authority) {
        put_str(&b, "//");
        if ((username != NULL && username->len > 0) || (password != NULL && password->len > 0)) {
            if (username != NULL) {
                encode_into(&b, username->chars, username->len, SET_USERINFO);
            }
            if (password != NULL && password->len > 0) {
                put_char(&b, ':');
                encode_into(&b, password->chars, password->len, SET_USERINFO);
            }
            put_char(&b, '@');
        }
        if (host != NULL) {
            put(&b, host->chars, host->len);
        }
        if (port != NULL && port->len > 0) {
            put_char(&b, ':');
            put(&b, port->chars, port->len);
        }
    }
    if (path != NULL && path->len > 0) {
        if (authority && path->chars[0] != '/') {
            put_char(&b, '/');
        }
        for (size_t i = 0; i < path->len; i++) {
            char c = path->chars[i];
            if (c == '?' || c == '#' || c == '\\') {
                encode_into(&b, &c, 1, SET_COMPONENT);
            } else {
                encode_into(&b, &c, 1, SET_PATH);
            }
        }
    }
    if (query != NULL && query->len > 0) {
        put_char(&b, '?');
        for (size_t i = 0; i < query->len; i++) {
            char c = query->chars[i];
            encode_into(&b, &c, 1, c == '#' ? SET_COMPONENT : SET_QUERY);
        }
    }
    if (fragment != NULL && fragment->len > 0) {
        put_char(&b, '#');
        encode_into(&b, fragment->chars, fragment->len, SET_FRAGMENT);
    }
    Url u;
    bool ok = parse_url(&u, b.items != NULL ? b.items : "", b.len);
    if (!ok) {
        ok = throw_url(n, fn, b.items != NULL ? b.items : "", b.len, &u);
    } else {
        Buf href = {0};
        put_href(&href, &u);
        *result = buf_value(&href);
        plum_array_free(&href);
    }
    url_free(&u);
    plum_array_free(&b);
    return ok;
}

/* join(base, ref): `ref` read relative to `base`, as a link in a page is. */
static bool url_join(ARGS) {
    (void)argc;
    const char *fn = "url.join";
    const PlumString *base_text = plum_as_string(args[0]);
    const PlumString *ref_text = plum_as_string(args[1]);
    Url base;
    if (!parse_url(&base, base_text->chars, base_text->len)) {
        url_free(&base);
        return throw_url(n, fn, base_text->chars, base_text->len, &base);
    }
    Buf ref = {0};
    clean_input(&ref, ref_text->chars, ref_text->len);
    const char *r = ref.items != NULL ? ref.items : "";
    size_t len = ref.len, colon;
    bool special = is_special(&base.scheme);
    Buf out = {0};
    if (valid_scheme(r, len, &colon)) {
        put(&out, r, len); /* already absolute */
    } else if (len >= 2 && is_slash(r[0], special) && is_slash(r[1], special)) {
        put(&out, base.scheme.items, base.scheme.len);
        put_char(&out, ':');
        put(&out, r, len);
    } else {
        size_t q = len, h = len;
        for (size_t k = 0; k < len; k++) {
            if (r[k] == '#') {
                h = k;
                break;
            }
            if (r[k] == '?' && q == len) {
                q = k;
            }
        }
        if (q > h) {
            q = h;
        }
        if (base.opaque && len > 0 && r[0] != '#') {
            Buf shown = {0};
            put_href(&shown, &base);
            bool done = plum_native_throw(n, PLUM_ERROR_VALUE,
                                          "%s: \"%.*s\" can't have a relative URL resolved "
                                          "against it, only a #fragment",
                                          fn, shown.len > 80 ? 80 : (int)shown.len, shown.items);
            plum_array_free(&shown);
            plum_array_free(&ref);
            url_free(&base);
            return done;
        }
        put(&out, base.scheme.items, base.scheme.len);
        put_char(&out, ':');
        if (base.has_host) {
            put_authority(&out, &base);
        }
        Buf path = {0};
        if (q > 0 && is_slash(r[0], special)) {
            put(&path, r, q);
        } else if (q > 0) {
            size_t keep = 0;
            for (size_t k = base.path.len; k > 0; k--) {
                if (base.path.items[k - 1] == '/') {
                    keep = k;
                    break;
                }
            }
            if (keep == 0 && base.has_host) {
                put_char(&path, '/');
            }
            put(&path, base.path.items, keep);
            put(&path, r, q);
        } else {
            put(&path, base.path.items, base.path.len);
        }
        if (!base.has_host && !base.opaque && path.len >= 2 && path.items[0] == '/' &&
            is_slash(path.items[1], special)) {
            put_str(&out, "/.");
        }
        put(&out, path.items, path.len);
        plum_array_free(&path);
        if (q < h || q > 0) {
            put(&out, r + q, len - q); /* the ref's own query, if any, and fragment */
        } else {
            if (base.has_query) {
                put_char(&out, '?');
                put(&out, base.query.items, base.query.len);
            }
            put(&out, r + h, len - h);
        }
    }
    url_free(&base);
    plum_array_free(&ref);
    Url u;
    bool ok = parse_url(&u, out.items != NULL ? out.items : "", out.len);
    if (!ok) {
        ok = throw_url(n, fn, ref_text->chars, ref_text->len, &u);
    } else {
        Buf href = {0};
        put_href(&href, &u);
        *result = buf_value(&href);
        plum_array_free(&href);
    }
    url_free(&u);
    plum_array_free(&out);
    return ok;
}

static bool url_encode(ARGS) {
    (void)n;
    (void)argc;
    const PlumString *s = plum_as_string(args[0]);
    Buf b = {0};
    encode_into(&b, s->chars, s->len, SET_COMPONENT);
    *result = buf_value(&b);
    plum_array_free(&b);
    return true;
}

static bool url_decode(ARGS) {
    (void)n;
    (void)argc;
    const PlumString *s = plum_as_string(args[0]);
    Buf b = {0};
    decode_into(&b, s->chars, s->len, false);
    *result = buf_value(&b);
    plum_array_free(&b);
    return true;
}

/* parse_query(text): name=value pairs split on &, + and %XX decoded; each
 * name's values in order. */
static bool url_parse_query(ARGS) {
    (void)argc;
    const PlumString *s = plum_as_string(args[0]);
    const char *p = s->chars;
    size_t len = s->len, i = 0;
    if (len > 0 && p[0] == '?') {
        i = 1;
    }
    PlumTable *map = plum_native_new_map(n);
    Buf name = {0}, value = {0};
    while (i <= len) {
        size_t end = i;
        while (end < len && p[end] != '&') {
            end++;
        }
        if (end > i) {
            size_t eq = i;
            while (eq < end && p[eq] != '=') {
                eq++;
            }
            name.len = value.len = 0;
            decode_into(&name, p + i, eq - i, true);
            if (eq < end) {
                decode_into(&value, p + eq + 1, end - eq - 1, true);
            }
            PlumValue key = buf_value(&name);
            PlumValue *slot = plum_table_find(map, key);
            if (slot == NULL) {
                PlumList *list = plum_list_new(1);
                plum_list_push(list, buf_value(&value));
                plum_table_put(map, key, plum_obj(&list->obj));
            } else {
                plum_release(key);
                plum_list_push(plum_as_list(*slot), buf_value(&value));
            }
        }
        i = end + 1;
    }
    plum_array_free(&name);
    plum_array_free(&value);
    *result = plum_obj(&map->obj);
    return true;
}

/* build_query(params): name=value pairs joined with &, form-encoded. */
static bool url_build_query(ARGS) {
    (void)n;
    (void)argc;
    const PlumTable *map = plum_as_table(args[0]);
    Buf b = {0};
    int64_t pos = 0;
    const PlumTableEntry *e;
    while ((e = plum_table_next(map, &pos)) != NULL) {
        const PlumString *key = plum_as_string(e->key);
        const PlumList *values = plum_as_list(e->value);
        for (size_t i = 0; i < values->len; i++) {
            const PlumString *v = plum_as_string(values->items[i]);
            if (b.len > 0) {
                put_char(&b, '&');
            }
            encode_into(&b, key->chars, key->len, SET_FORM);
            put_char(&b, '=');
            encode_into(&b, v->chars, v->len, SET_FORM);
        }
    }
    *result = buf_value(&b);
    plum_array_free(&b);
    return true;
}

static const PlumNativeFunction functions[] = {
    {"parse", "(text: str) -> map[str, str]", url_parse,
     "the parts of a URL: scheme, username, password, host, port, path, query, fragment, and "
     "href (the whole URL, normalized); a missing part is \"\""},
    {"build", "(parts: map[str, str]) -> str", url_build,
     "a URL from parts like parse gives (scheme needed, the rest optional), each "
     "percent-encoded as it needs"},
    {"normalize", "(text: str) -> str", url_normalize,
     "the URL written as parse reads it: scheme and host in small letters, . and .. resolved, "
     "the default port dropped"},
    {"join", "(base: str, ref: str) -> str", url_join,
     "ref read relative to base, as a browser follows a link: \"../b\", \"/c\", \"?q\", "
     "\"#f\", \"//host/x\" or a whole URL"},
    {"encode", "(text: str) -> str", url_encode,
     "percent-encode every byte but A-Z a-z 0-9 - . _ ~, for one part of a path or query"},
    {"decode", "(text: str) -> str", url_decode,
     "%XX back to bytes (+ stays +); a % without two hex digits is kept"},
    {"parse_query", "(text: str) -> map[str, list[str]]", url_parse_query,
     "a query string (with or without ?) as each name's values in order; + is a space"},
    {"build_query", "(params: map[str, list[str]]) -> str", url_build_query,
     "name=value pairs joined by &, form-encoded (a space is +), in the map's order"},
};

const PlumNativeModule plum_module_url = {
    "url",     "parse, build and resolve URLs; percent-encoding and query strings",
    functions, sizeof functions / sizeof functions[0],
    NULL,      0,
};
