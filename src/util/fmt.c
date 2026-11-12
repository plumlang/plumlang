/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "util/fmt.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

char *plum_format_float(double value, char buf[PLUM_FLOAT_BUF]) {
    if (isnan(value)) {
        strcpy(buf, "nan");
        return buf;
    }
    if (isinf(value)) {
        strcpy(buf, value > 0 ? "inf" : "-inf");
        return buf;
    }

    /* The fewest significant digits that read back as the same value. */
    char sci[PLUM_FLOAT_BUF];
    for (int precision = 1; precision <= 17; precision++) {
        snprintf(sci, sizeof sci, "%.*e", precision - 1, value);
        if (strtod(sci, NULL) == value) {
            break;
        }
    }

    /* Split "-d.ddde+XX" into sign, digits and decimal exponent. */
    const char *s = sci;
    bool negative = *s == '-';
    if (negative) {
        s++;
    }
    char digits[20];
    size_t ndigits = 0;
    for (; *s != 'e'; s++) {
        if (*s != '.') {
            digits[ndigits++] = *s;
        }
    }
    int exponent = atoi(s + 1);
    while (ndigits > 1 && digits[ndigits - 1] == '0') {
        ndigits--; /* trailing zeros carry no information */
    }

    char *out = buf;
    if (negative) {
        *out++ = '-';
    }
    if (exponent >= -4 && exponent < 16) {
        /* Positional, like Python's repr: 0.0025, 1000000000.0 */
        if (exponent < 0) {
            *out++ = '0';
            *out++ = '.';
            for (int i = -1; i > exponent; i--) {
                *out++ = '0';
            }
            memcpy(out, digits, ndigits);
            out += ndigits;
        } else {
            size_t whole = (size_t)exponent + 1;
            for (size_t i = 0; i < whole; i++) {
                *out++ = i < ndigits ? digits[i] : '0';
            }
            *out++ = '.';
            if (ndigits > whole) {
                memcpy(out, digits + whole, ndigits - whole);
                out += ndigits - whole;
            } else {
                *out++ = '0';
            }
        }
        *out = '\0';
    } else {
        /* Scientific: 1e+20, 6.022e+23, 1e-05 */
        *out++ = digits[0];
        if (ndigits > 1) {
            *out++ = '.';
            memcpy(out, digits + 1, ndigits - 1);
            out += ndigits - 1;
        }
        snprintf(out, PLUM_FLOAT_BUF - (size_t)(out - buf), "e%c%02d", exponent < 0 ? '-' : '+',
                 exponent < 0 ? -exponent : exponent);
    }
    return buf;
}

void plum_write_quoted(FILE *out, const char *text, size_t len) {
    fputc('"', out);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        switch (c) {
        case '\n':
            fputs("\\n", out);
            break;
        case '\t':
            fputs("\\t", out);
            break;
        case '\r':
            fputs("\\r", out);
            break;
        case '\0':
            fputs("\\0", out);
            break;
        case '"':
            fputs("\\\"", out);
            break;
        case '\\':
            fputs("\\\\", out);
            break;
        case '{':
            fputs("\\{", out);
            break;
        case '}':
            fputs("\\}", out);
            break;
        default:
            if (c < 0x20 || c == 0x7F) {
                fprintf(out, "\\u{%X}", (unsigned)c);
            } else {
                fputc((int)c, out);
            }
        }
    }
    fputc('"', out);
}
