/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The base64 module: bytes as Base64 text (RFC 4648) and back, in the
 * standard alphabet (with = padding) and the URL- and file-safe one
 * (- and _, no padding). Decoding is strict: whitespace, characters of the
 * other alphabet and wrong padding are errors.
 */
#include "stdlib/native.h"

#include <string.h>

#define ARGS PlumNative *n, const PlumValue *args, int argc, PlumValue *result

static const char standard[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char url_safe[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static bool encode(PlumNative *n, const PlumString *s, bool url, PlumValue *result) {
    const char *alphabet = url ? url_safe : standard;
    size_t groups = s->len / 3 + (s->len % 3 != 0);
    if (groups > PLUM_MAX_STRING / 4) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "%s: the text would be longer than %zu bytes, the most a str can "
                                 "hold",
                                 url ? "base64.encode_url" : "base64.encode", PLUM_MAX_STRING);
    }
    size_t len = groups * 4;
    if (url && s->len % 3 != 0) {
        len -= 3 - s->len % 3; /* no padding */
    }
    PlumString *out = plum_string_alloc(len);
    const unsigned char *in = (const unsigned char *)s->chars;
    size_t o = 0;
    for (size_t i = 0; i < s->len; i += 3) {
        size_t left = s->len - i;
        uint32_t v = (uint32_t)in[i] << 16;
        if (left > 1) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        if (left > 2) {
            v |= in[i + 2];
        }
        out->chars[o++] = alphabet[(v >> 18) & 0x3F];
        out->chars[o++] = alphabet[(v >> 12) & 0x3F];
        if (left > 1) {
            out->chars[o++] = alphabet[(v >> 6) & 0x3F];
        } else if (!url) {
            out->chars[o++] = '=';
        }
        if (left > 2) {
            out->chars[o++] = alphabet[v & 0x3F];
        } else if (!url) {
            out->chars[o++] = '=';
        }
    }
    *result = plum_obj(&out->obj);
    return true;
}

/* The value of Base64 digit `c` in the alphabet, or -1. */
static int digit(char c, bool url) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == (url ? '-' : '+')) {
        return 62;
    }
    if (c == (url ? '_' : '/')) {
        return 63;
    }
    return -1;
}

static bool decode(PlumNative *n, const PlumString *s, bool url, PlumValue *result) {
    const char *name = url ? "base64.decode_url" : "base64.decode";
    /* The digits end where the padding starts. */
    size_t len = s->len;
    size_t pad = 0;
    while (len > 0 && s->chars[len - 1] == '=' && pad < 2) {
        len--;
        pad++;
    }
    for (size_t i = 0; i < len; i++) {
        if (digit(s->chars[i], url) < 0) {
            unsigned char c = (unsigned char)s->chars[i];
            if (c == '=') {
                return plum_native_throw(n, PLUM_ERROR_VALUE,
                                         "%s: '=' at position %zu; padding goes only at the end, "
                                         "at most two",
                                         name, i);
            }
            if (c < 0x20 || c >= 0x7F) {
                return plum_native_throw(n, PLUM_ERROR_VALUE,
                                         "%s: byte 0x%02X at position %zu isn't a Base64 digit",
                                         name, c, i);
            }
            return plum_native_throw(n, PLUM_ERROR_VALUE,
                                     "%s: '%c' at position %zu isn't a Base64 digit%s", name,
                                     (char)c, i,
                                     url ? " of the URL alphabet (A-Z a-z 0-9 - _)"
                                         : " (A-Z a-z 0-9 + /)");
        }
    }
    if (len % 4 == 1) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "%s: the last group has one digit; Base64 needs two or more",
                                 name);
    }
    /* With padding, the text comes in whole groups of four. */
    if (pad > 0 && (len + pad) % 4 != 0) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "%s: wrong padding: %zu '=' after %zu digits", name, pad, len);
    }
    if (!url && pad == 0 && len % 4 != 0) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "%s: the padding is missing: %zu digits need %zu '='; "
                                 "base64.decode_url accepts text without it",
                                 name, len, 4 - len % 4);
    }
    size_t out_len = len / 4 * 3 + (len % 4 == 0 ? 0 : len % 4 - 1);
    PlumString *out = plum_string_alloc(out_len);
    size_t o = 0;
    for (size_t i = 0; i < len; i += 4) {
        size_t left = len - i < 4 ? len - i : 4;
        uint32_t v = 0;
        for (size_t k = 0; k < 4; k++) {
            v = (v << 6) | (k < left ? (uint32_t)digit(s->chars[i + k], url) : 0);
        }
        out->chars[o++] = (char)((v >> 16) & 0xFF);
        if (left > 2) {
            out->chars[o++] = (char)((v >> 8) & 0xFF);
        }
        if (left > 3) {
            out->chars[o++] = (char)(v & 0xFF);
        }
    }
    *result = plum_obj(&out->obj);
    return true;
}

static bool b64_encode(ARGS) {
    (void)argc;
    return encode(n, plum_as_string(args[0]), false, result);
}

static bool b64_decode(ARGS) {
    (void)argc;
    return decode(n, plum_as_string(args[0]), false, result);
}

static bool b64_encode_url(ARGS) {
    (void)argc;
    return encode(n, plum_as_string(args[0]), true, result);
}

static bool b64_decode_url(ARGS) {
    (void)argc;
    return decode(n, plum_as_string(args[0]), true, result);
}

static const PlumNativeFunction functions[] = {
    {"encode", "(data: str) -> str", b64_encode,
     "the bytes as Base64, standard alphabet, with = padding"},
    {"decode", "(text: str) -> str", b64_decode,
     "standard Base64 back to bytes; ValueError on a bad digit or padding"},
    {"encode_url", "(data: str) -> str", b64_encode_url,
     "the bytes as URL-safe Base64 (- and _ for + and /), without padding"},
    {"decode_url", "(text: str) -> str", b64_decode_url,
     "URL-safe Base64 back to bytes, with or without padding"},
};

const PlumNativeModule plum_module_base64 = {
    "base64",  "bytes as Base64 text (RFC 4648) and back",
    functions, sizeof functions / sizeof functions[0],
    NULL,      0,
};
