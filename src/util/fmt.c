/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "util/fmt.h"

#include <math.h>
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
    /* The fewest significant digits that round-trip; 17 always does. */
    for (int precision = 1; precision <= 17; precision++) {
        snprintf(buf, PLUM_FLOAT_BUF, "%.*g", precision, value);
        if (strtod(buf, NULL) == value) {
            break;
        }
    }
    if (strpbrk(buf, ".e") == NULL) {
        strcat(buf, ".0"); /* "3" -> "3.0": a float always looks like one */
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
