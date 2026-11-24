/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The uuid module: universally unique identifiers (RFC 9562) as text,
 * "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" in small letters.
 *
 * v4 is 122 random bits; v7 is the Unix time in milliseconds followed by
 * 74 random bits. The random bits come from the crypto library's
 * generator (vendored Mbed TLS, PSA API), which the operating system's
 * secure source seeds: never from math.random's generator, which a
 * program can seed.
 *
 * v7 identifiers made by one program always increase: within the same
 * millisecond (or when the clock goes back) the 74 bits after the time
 * count up from the last one by a random step (RFC 9562 6.2, method 2).
 */
#include "stdlib/native.h"

#include "psa/crypto.h"
#include "stdlib/platform.h"
#include "util/alloc.h"

#include <math.h>
#include <string.h>

#define ARGS PlumNative *n, const PlumValue *args, int argc, PlumValue *result

typedef struct Uuid {
    uint8_t b[16];
} Uuid;

/* The last v7 made: its milliseconds and its 74 bits (12 in a, 62 in b). */
typedef struct V7State {
    int64_t ms;
    uint32_t a;
    uint64_t b;
} V7State;

static const char v7_key = 0;

static bool secure_random(PlumNative *n, const char *fn, uint8_t *out, size_t len) {
    psa_status_t status = psa_crypto_init(); /* once per process; later calls do nothing */
    if (status == PSA_SUCCESS) {
        status = psa_generate_random(out, len);
    }
    if (status != PSA_SUCCESS) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "uuid.%s: the system's secure random source failed (%d)", fn,
                                 (int)status);
    }
    return true;
}

static PlumValue format(const Uuid *u) {
    static const char hex[] = "0123456789abcdef";
    char out[36];
    size_t at = 0;
    for (size_t i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out[at++] = '-';
        }
        out[at++] = hex[u->b[i] >> 4];
        out[at++] = hex[u->b[i] & 0x0F];
    }
    return plum_str_value(out, sizeof out);
}

static int hex_digit(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

/*
 * Read a UUID: 32 hex digits, either case, with hyphens after the 8th,
 * 12th, 16th and 20th or none at all; optionally in {braces} or after
 * "urn:uuid:".
 */
static bool scan(const PlumString *s, Uuid *out) {
    const char *p = s->chars;
    size_t len = s->len;
    if (len >= 9 && (strncmp(p, "urn:uuid:", 9) == 0 || strncmp(p, "URN:UUID:", 9) == 0)) {
        p += 9;
        len -= 9;
    } else if (len >= 2 && p[0] == '{' && p[len - 1] == '}') {
        p++;
        len -= 2;
    }
    bool hyphens = len == 36;
    if (!hyphens && len != 32) {
        return false;
    }
    size_t at = 0;
    for (size_t i = 0; i < 16; i++) {
        if (hyphens && (i == 4 || i == 6 || i == 8 || i == 10)) {
            if (p[at++] != '-') {
                return false;
            }
        }
        int hi = hex_digit(p[at]), lo = hex_digit(p[at + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out->b[i] = (uint8_t)(hi * 16 + lo);
        at += 2;
    }
    return true;
}

static bool read_arg(PlumNative *n, const char *fn, PlumValue v, Uuid *out) {
    const PlumString *s = plum_as_string(v);
    if (scan(s, out)) {
        return true;
    }
    if (s->len <= 60 && memchr(s->chars, '\0', s->len) == NULL) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "uuid.%s: \"%.*s\" isn't a UUID; write 32 hex digits like "
                                 "\"0190b6a4-5c3e-7d2a-9f1b-3c4d5e6f7a8b\"",
                                 fn, (int)s->len, s->chars);
    }
    return plum_native_throw(n, PLUM_ERROR_VALUE,
                             "uuid.%s: a text of %zu bytes isn't a UUID; write 32 hex digits like "
                             "\"0190b6a4-5c3e-7d2a-9f1b-3c4d5e6f7a8b\"",
                             fn, s->len);
}

static void set_version(Uuid *u, unsigned version) {
    u->b[6] = (uint8_t)((u->b[6] & 0x0F) | (version << 4));
    u->b[8] = (uint8_t)((u->b[8] & 0x3F) | 0x80); /* the variant: 10 */
}

static bool u_v4(ARGS) {
    (void)args;
    (void)argc;
    Uuid u;
    if (!secure_random(n, "v4", u.b, sizeof u.b)) {
        return false;
    }
    set_version(&u, 4);
    *result = format(&u);
    return true;
}

#define V7_B_LIMIT ((uint64_t)1 << 62)

static bool u_v7(ARGS) {
    (void)args;
    (void)argc;
    uint8_t r[12];
    if (!secure_random(n, "v7", r, sizeof r)) {
        return false;
    }
    uint32_t a = (uint32_t)(((unsigned)r[0] << 8 | r[1]) & 0x0FFF);
    uint64_t b = 0;
    for (int i = 2; i < 10; i++) {
        b = b << 8 | r[i];
    }
    b &= V7_B_LIMIT - 1;
    uint64_t step = 1 + (((uint64_t)r[10] << 8 | r[11]) << 8); /* 1 to about 2^24 */

    double now = floor(plum_os_now() * 1000.0);
    int64_t ms = now < 0 ? 0 : now >= 281474976710655.0 ? 281474976710655 : (int64_t)now;
    void **slot = plum_native_program_data(n, &v7_key, plum_free);
    V7State *last = *slot;
    if (last == NULL) {
        last = plum_alloc_zeroed(1, sizeof *last);
        last->ms = -1;
        *slot = last;
    }
    if (ms <= last->ms) {
        /* the same millisecond, or the clock went back: count on from the last */
        ms = last->ms;
        a = last->a;
        b = last->b + step;
        if (b >= V7_B_LIMIT) {
            b -= V7_B_LIMIT;
            a++;
        }
        if (a > 0x0FFF) {
            /* the 74 bits ran out: borrow the next millisecond */
            ms++;
            a = (uint32_t)(((unsigned)r[0] << 8 | r[1]) & 0x07FF);
            b = 0;
        }
    }
    last->ms = ms;
    last->a = a;
    last->b = b;

    Uuid u;
    for (int i = 0; i < 6; i++) {
        u.b[i] = (uint8_t)((uint64_t)ms >> (40 - 8 * i));
    }
    u.b[6] = (uint8_t)(a >> 8);
    u.b[7] = (uint8_t)a;
    for (int i = 0; i < 8; i++) {
        u.b[8 + i] = (uint8_t)(b >> (56 - 8 * i));
    }
    set_version(&u, 7);
    *result = format(&u);
    return true;
}

static bool u_parse(ARGS) {
    (void)argc;
    Uuid u;
    if (!read_arg(n, "parse", args[0], &u)) {
        return false;
    }
    *result = format(&u);
    return true;
}

static bool u_is_valid(ARGS) {
    (void)n;
    (void)argc;
    Uuid u;
    *result = plum_bool(scan(plum_as_string(args[0]), &u));
    return true;
}

static bool u_version(ARGS) {
    (void)argc;
    Uuid u;
    if (!read_arg(n, "version", args[0], &u)) {
        return false;
    }
    *result = plum_int(u.b[6] >> 4);
    return true;
}

static bool u_variant(ARGS) {
    (void)argc;
    Uuid u;
    if (!read_arg(n, "variant", args[0], &u)) {
        return false;
    }
    unsigned v = u.b[8];
    const char *name = (v & 0x80) == 0      ? "ncs"
                       : (v & 0xC0) == 0x80 ? "rfc"
                       : (v & 0xE0) == 0xC0 ? "microsoft"
                                            : "future";
    *result = plum_str_value(name, strlen(name));
    return true;
}

static bool u_compare(ARGS) {
    (void)argc;
    Uuid a, b;
    if (!read_arg(n, "compare", args[0], &a) || !read_arg(n, "compare", args[1], &b)) {
        return false;
    }
    int c = memcmp(a.b, b.b, sizeof a.b);
    *result = plum_int(c < 0 ? -1 : c > 0 ? 1 : 0);
    return true;
}

static bool u_timestamp(ARGS) {
    (void)argc;
    Uuid u;
    if (!read_arg(n, "timestamp", args[0], &u)) {
        return false;
    }
    if (u.b[6] >> 4 != 7) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "uuid.timestamp: this is a version %d UUID; only version 7 "
                                 "holds a Unix time",
                                 u.b[6] >> 4);
    }
    int64_t ms = 0;
    for (int i = 0; i < 6; i++) {
        ms = ms << 8 | u.b[i];
    }
    *result = plum_int(ms);
    return true;
}

static const PlumNativeFunction functions[] = {
    {"v4", "() -> str", u_v4, "a random UUID (version 4), from the system's secure random source"},
    {"v7", "() -> str", u_v7,
     "a time-ordered UUID (version 7): the Unix time in milliseconds, then random bits; each "
     "one a program makes sorts after the one before"},
    {"parse", "(text: str) -> str", u_parse,
     "a UUID in its usual form, 36 characters in small letters; takes either case, no hyphens, "
     "{braces} or urn:uuid:"},
    {"is_valid", "(text: str) -> bool", u_is_valid, "would parse accept the text?"},
    {"version", "(id: str) -> int", u_version, "the version number, 0 to 15 (4 for v4, 7 for v7)"},
    {"variant", "(id: str) -> str", u_variant,
     "the variant: \"rfc\" (RFC 9562, as v4 and v7 make), \"ncs\", \"microsoft\" or \"future\""},
    {"compare", "(a: str, b: str) -> int", u_compare,
     "-1, 0 or 1 as a sorts before, with or after b, byte by byte (v7 by time)"},
    {"timestamp", "(id: str) -> int", u_timestamp,
     "a version 7 UUID's time, in milliseconds since 1970-01-01 00:00 UTC"},
};

static const PlumNativeConstant constants[] = {
    {"NIL", PLUM_NATIVE_STR, 0, 0.0, "00000000-0000-0000-0000-000000000000",
     "the nil UUID, all zero bits"},
    {"MAX", PLUM_NATIVE_STR, 0, 0.0, "ffffffff-ffff-ffff-ffff-ffffffffffff",
     "the max UUID, all one bits"},
};

const PlumNativeModule plum_module_uuid = {
    "uuid",    "unique identifiers (RFC 9562): random v4 and time-ordered v7",
    functions, sizeof functions / sizeof functions[0],
    constants, sizeof constants / sizeof constants[0],
};
