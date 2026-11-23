/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The utf8 module: characters as code points, and checking and repairing
 * UTF-8. Decoding is strict (RFC 3629): no overlong forms, no surrogates
 * (U+D800 to U+DFFF), nothing above U+10FFFF. fix() replaces each maximal
 * ill-formed part with U+FFFD, as the Unicode Standard (section 3.9) and
 * the WHATWG decoder do.
 */
#include "stdlib/native.h"

#include "util/array.h"
#include "vm/text.h"

#include <stdio.h>
#include <string.h>

#define ARGS PlumNative *n, const PlumValue *args, int argc, PlumValue *result

#define MAX_CODE 0x10FFFF

typedef PLUM_ARRAY(char) Buf;

/*
 * Decode the character at s[0..len), len > 0. Returns its length in bytes
 * with its code point in *cp, or 0 if the bytes there aren't valid UTF-8;
 * then *bad is how many bytes make up the ill-formed part (at least 1).
 */
static size_t decode(const unsigned char *s, size_t len, uint32_t *cp, size_t *bad) {
    unsigned char c = s[0];
    size_t need;
    unsigned char lo = 0x80, hi = 0xBF; /* the allowed range of the second byte */
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if (c < 0xC2) { /* a continuation byte, or the start of an overlong form */
        *bad = 1;
        return 0;
    }
    if (c < 0xE0) {
        need = 1;
        *cp = c & 0x1Fu;
    } else if (c < 0xF0) {
        need = 2;
        lo = c == 0xE0 ? 0xA0 : 0x80; /* not overlong */
        hi = c == 0xED ? 0x9F : 0xBF; /* not a surrogate */
        *cp = c & 0x0Fu;
    } else if (c < 0xF5) {
        need = 3;
        lo = c == 0xF0 ? 0x90 : 0x80; /* not overlong */
        hi = c == 0xF4 ? 0x8F : 0xBF; /* not above U+10FFFF */
        *cp = c & 0x07u;
    } else {
        *bad = 1;
        return 0;
    }
    for (size_t k = 1; k <= need; k++) {
        unsigned char low = k == 1 ? lo : 0x80, high = k == 1 ? hi : 0xBF;
        if (k >= len || s[k] < low || s[k] > high) {
            *bad = k;
            return 0;
        }
        *cp = (*cp << 6) | (s[k] & 0x3Fu);
    }
    return need + 1;
}

/* The UTF-8 of code point `cp` (valid) in out; returns its length. */
static size_t encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Is `code` a Unicode scalar value, one UTF-8 can hold? */
static bool is_scalar(int64_t code) {
    return code >= 0 && code <= MAX_CODE && !(code >= 0xD800 && code <= 0xDFFF);
}

static bool code_error(PlumNative *n, const char *fn, int64_t code) {
    if (code >= 0xD800 && code <= 0xDFFF) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "%s: %lld (0x%llX) is a surrogate, which UTF-8 can't hold", fn,
                                 (long long)code, (unsigned long long)code);
    }
    return plum_native_throw(n, PLUM_ERROR_VALUE,
                             "%s: %lld is no Unicode code point; they go from 0 to 1114111 "
                             "(0x10FFFF)",
                             fn, (long long)code);
}

static bool invalid_error(PlumNative *n, const char *fn, size_t at) {
    return plum_native_throw(n, PLUM_ERROR_VALUE,
                             "%s: the text isn't valid UTF-8 at byte %zu; utf8.fix() replaces "
                             "the bad bytes",
                             fn, at);
}

/* code(ch): the code point of a one-character string. */
static bool u_code(ARGS) {
    (void)argc;
    const PlumString *s = plum_as_string(args[0]);
    if (s->len == 0) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "utf8.code: the text is empty; give exactly one character");
    }
    uint32_t cp = 0;
    size_t bad = 0;
    size_t k = decode((const unsigned char *)s->chars, s->len, &cp, &bad);
    if (k == 0) {
        return invalid_error(n, "utf8.code", 0);
    }
    if (k != s->len) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "utf8.code: the text has %zu characters; give exactly one",
                                 plum_utf8_length(s->chars, s->len));
    }
    *result = plum_int((int64_t)cp);
    return true;
}

/* char(code): the character with that code point. */
static bool u_char(ARGS) {
    (void)argc;
    int64_t code = args[0].as.i;
    if (!is_scalar(code)) {
        return code_error(n, "utf8.char", code);
    }
    char out[4];
    *result = plum_str_value(out, encode((uint32_t)code, out));
    return true;
}

/* codes(text): every character's code point. */
static bool u_codes(ARGS) {
    (void)argc;
    const PlumString *s = plum_as_string(args[0]);
    const unsigned char *b = (const unsigned char *)s->chars;
    PlumList *out = plum_list_new(0);
    for (size_t i = 0; i < s->len;) {
        uint32_t cp = 0;
        size_t bad = 0;
        size_t k = decode(b + i, s->len - i, &cp, &bad);
        if (k == 0) {
            plum_release(plum_obj(&out->obj));
            return invalid_error(n, "utf8.codes", i);
        }
        if (out->len >= PLUM_MAX_LIST) {
            plum_release(plum_obj(&out->obj));
            return plum_native_throw(n, PLUM_ERROR_VALUE,
                                     "utf8.codes: the list would be longer than %zu items, the "
                                     "most a list can hold",
                                     PLUM_MAX_LIST);
        }
        plum_list_push(out, plum_int((int64_t)cp));
        i += k;
    }
    *result = plum_obj(&out->obj);
    return true;
}

/* from_codes(codes): the text of those code points. */
static bool u_from_codes(ARGS) {
    (void)argc;
    const PlumList *codes = plum_as_list(args[0]);
    Buf b = {0};
    for (size_t i = 0; i < codes->len; i++) {
        int64_t code = codes->items[i].as.i;
        if (!is_scalar(code)) {
            plum_array_free(&b);
            char fn[64];
            snprintf(fn, sizeof fn, "utf8.from_codes: item %zu", i);
            return code_error(n, fn, code);
        }
        char enc[4];
        size_t k = encode((uint32_t)code, enc);
        plum_array_reserve(&b, k);
        memcpy(b.items + b.len, enc, k);
        b.len += k;
    }
    *result = plum_str_value(b.items != NULL ? b.items : "", b.len);
    plum_array_free(&b);
    return true;
}

static bool u_is_valid(ARGS) {
    (void)n;
    (void)argc;
    const PlumString *s = plum_as_string(args[0]);
    const unsigned char *b = (const unsigned char *)s->chars;
    bool ok = true;
    for (size_t i = 0; i < s->len && ok;) {
        uint32_t cp = 0;
        size_t bad = 0;
        size_t k = decode(b + i, s->len - i, &cp, &bad);
        ok = k > 0;
        i += k;
    }
    *result = plum_bool(ok);
    return true;
}

/* fix(text): valid UTF-8, each ill-formed part replaced with U+FFFD. */
static bool u_fix(ARGS) {
    (void)argc;
    static const char replacement[] = "\xEF\xBF\xBD";
    const PlumString *s = plum_as_string(args[0]);
    const unsigned char *in = (const unsigned char *)s->chars;
    Buf b = {0};
    size_t run = 0; /* where the valid bytes not yet copied start */
    for (size_t i = 0; i < s->len;) {
        uint32_t cp = 0;
        size_t bad = 0;
        size_t k = decode(in + i, s->len - i, &cp, &bad);
        if (k > 0) {
            i += k;
            continue;
        }
        /* copy the valid run before it, then one U+FFFD */
        size_t add = i - run + 3;
        if (b.len + add > PLUM_MAX_STRING) {
            plum_array_free(&b);
            return plum_native_throw(n, PLUM_ERROR_VALUE,
                                     "utf8.fix: the text would be longer than %zu bytes, the "
                                     "most a str can hold",
                                     PLUM_MAX_STRING);
        }
        plum_array_reserve(&b, add);
        memcpy(b.items + b.len, s->chars + run, i - run);
        memcpy(b.items + b.len + (i - run), replacement, 3);
        b.len += add;
        i += bad;
        run = i;
    }
    if (run == 0) { /* nothing to replace: the same text */
        plum_array_free(&b);
        *result = plum_retain(args[0]);
        return true;
    }
    if (b.len + (s->len - run) > PLUM_MAX_STRING) {
        plum_array_free(&b);
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "utf8.fix: the text would be longer than %zu bytes, the most "
                                 "a str can hold",
                                 PLUM_MAX_STRING);
    }
    plum_array_reserve(&b, s->len - run);
    memcpy(b.items + b.len, s->chars + run, s->len - run);
    b.len += s->len - run;
    *result = plum_str_value(b.items, b.len);
    plum_array_free(&b);
    return true;
}

static const PlumNativeFunction functions[] = {
    {"code", "(ch: str) -> int", u_code, "the code point of a one-character string"},
    {"char", "(code: int) -> str", u_char,
     "the character with that code point (0 to 0x10FFFF, no surrogates)"},
    {"codes", "(text: str) -> list[int]", u_codes, "the code point of every character"},
    {"from_codes", "(codes: list[int]) -> str", u_from_codes, "the text of those code points"},
    {"is_valid", "(text: str) -> bool", u_is_valid,
     "is the text strict UTF-8 (no overlong forms, surrogates or code points past 0x10FFFF)?"},
    {"fix", "(text: str) -> str", u_fix,
     "valid UTF-8: each invalid sequence replaced with U+FFFD"},
};

const PlumNativeModule plum_module_utf8 = {
    "utf8",    "characters as code points; checking and repairing UTF-8",
    functions, sizeof functions / sizeof functions[0],
    NULL,      0,
};
