/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/* The hex module: bytes as hexadecimal text, two digits a byte, and back. */
#include "stdlib/native.h"

#define ARGS PlumNative *n, const PlumValue *args, int argc, PlumValue *result

/* encode(data, upper = bad): two digits for each byte of the text. */
static bool hex_encode(ARGS) {
    const PlumString *s = plum_as_string(args[0]);
    bool upper = argc > 1 && plum_is_bool(args[1]) && plum_as_bool(args[1]);
    if (s->len > PLUM_MAX_STRING / 2) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "hex.encode: the text would be longer than %zu bytes, the most "
                                 "a str can hold",
                                 PLUM_MAX_STRING);
    }
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    PlumString *out = plum_string_alloc(s->len * 2);
    for (size_t i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->chars[i];
        out->chars[2 * i] = digits[c >> 4];
        out->chars[2 * i + 1] = digits[c & 0x0F];
    }
    *result = plum_obj(&out->obj);
    return true;
}

static int hex_digit(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

/* decode(text): pairs of hex digits, either case, back to bytes. */
static bool hex_decode(ARGS) {
    (void)argc;
    const PlumString *s = plum_as_string(args[0]);
    for (size_t i = 0; i < s->len; i++) {
        if (hex_digit(s->chars[i]) < 0) {
            unsigned char c = (unsigned char)s->chars[i];
            if (c < 0x20 || c >= 0x7F) {
                return plum_native_throw(n, PLUM_ERROR_VALUE,
                                         "hex.decode: byte 0x%02X at position %zu isn't a hex "
                                         "digit",
                                         c, i);
            }
            return plum_native_throw(n, PLUM_ERROR_VALUE,
                                     "hex.decode: '%c' at position %zu isn't a hex digit", (char)c,
                                     i);
        }
    }
    if (s->len % 2 != 0) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "hex.decode: the text has an odd number of digits (%zu); each "
                                 "byte takes two",
                                 s->len);
    }
    PlumString *out = plum_string_alloc(s->len / 2);
    for (size_t i = 0; i < s->len / 2; i++) {
        out->chars[i] = (char)(hex_digit(s->chars[2 * i]) * 16 + hex_digit(s->chars[2 * i + 1]));
    }
    *result = plum_obj(&out->obj);
    return true;
}

static const PlumNativeFunction functions[] = {
    {"encode", "(data: str, upper: bool = bad) -> str", hex_encode,
     "two hex digits for each byte, small letters unless upper"},
    {"decode", "(text: str) -> str", hex_decode, "pairs of hex digits (either case) back to bytes"},
};

const PlumNativeModule plum_module_hex = {
    "hex",     "bytes as hexadecimal text and back",
    functions, sizeof functions / sizeof functions[0],
    NULL,      0,
};
