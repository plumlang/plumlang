/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "lsp/text.h"

#include "util/alloc.h"
#include "util/array.h"

#include <stdbool.h>
#include <string.h>

void plum_lsp_lines_init(PlumLspLines *lines, const char *text, size_t len) {
    PLUM_ARRAY(size_t) starts = {0};
    plum_array_push(&starts, (size_t)0);
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n' || (text[i] == '\r' && (i + 1 >= len || text[i + 1] != '\n'))) {
            plum_array_push(&starts, i + 1);
        }
    }
    lines->text = text;
    lines->len = len;
    lines->starts = starts.items;
    lines->count = starts.len;
}

void plum_lsp_lines_free(PlumLspLines *lines) {
    plum_free(lines->starts);
    memset(lines, 0, sizeof *lines);
}

/* Bytes in the UTF-8 character at s[0..n): 1 to 4; 1 for an invalid byte. */
static size_t char_bytes(const unsigned char *s, size_t n) {
    size_t len = s[0] < 0x80             ? 1
                 : (s[0] & 0xE0) == 0xC0 ? 2
                 : (s[0] & 0xF0) == 0xE0 ? 3
                 : (s[0] & 0xF8) == 0xF0 ? 4
                                         : 1;
    if (len > n) {
        return 1;
    }
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            return 1;
        }
    }
    return len;
}

/* The end of line `i`'s text, before its line break. */
static size_t line_end(const PlumLspLines *lines, size_t i) {
    size_t end = i + 1 < lines->count ? lines->starts[i + 1] : lines->len;
    if (end > lines->starts[i] && lines->text[end - 1] == '\n') {
        end--;
    }
    if (end > lines->starts[i] && lines->text[end - 1] == '\r') {
        end--;
    }
    return end;
}

size_t plum_lsp_offset(const PlumLspLines *lines, long long line, long long character) {
    if (line < 0) {
        return 0;
    }
    if ((unsigned long long)line >= lines->count) {
        return lines->len;
    }
    size_t i = lines->starts[line];
    size_t end = line_end(lines, (size_t)line);
    const unsigned char *u = (const unsigned char *)lines->text;
    long long units = 0;
    while (i < end && units < character) {
        size_t n = char_bytes(u + i, end - i);
        units += n == 4 ? 2 : 1;
        i += n;
    }
    return i;
}

void plum_lsp_position(const PlumLspLines *lines, size_t offset, long long *line,
                       long long *character) {
    if (offset > lines->len) {
        offset = lines->len;
    }
    /* the last line starting at or before offset */
    size_t lo = 0, hi = lines->count;
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (lines->starts[mid] <= offset) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    const unsigned char *u = (const unsigned char *)lines->text;
    size_t end = line_end(lines, lo);
    if (offset > end) {
        offset = end; /* inside a "\r\n": the end of the line */
    }
    long long units = 0;
    for (size_t i = lines->starts[lo]; i < offset;) {
        size_t n = char_bytes(u + i, offset - i);
        units += n == 4 ? 2 : 1;
        i += n;
    }
    *line = (long long)lo;
    *character = units;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

char *plum_lsp_uri_to_path(const char *uri, size_t len) {
    if (len < 7 || strncmp(uri, "file://", 7) != 0) {
        return plum_strndup(uri, len);
    }
    const char *p = uri + 7;
    const char *end = uri + len;
    /* file://host/path: a host other than localhost is kept as //host */
    char *out = plum_alloc(len + 3);
    size_t n = 0;
    if (p < end && *p != '/') {
        const char *slash = memchr(p, '/', (size_t)(end - p));
        size_t host = slash != NULL ? (size_t)(slash - p) : (size_t)(end - p);
        if (!(host == 9 && strncmp(p, "localhost", 9) == 0)) {
            out[n++] = '/';
            out[n++] = '/';
            memcpy(out + n, p, host);
            n += host;
        }
        p += host;
    }
    for (; p < end; p++) {
        if (*p == '%' && end - p >= 3 && hex_value(p[1]) >= 0 && hex_value(p[2]) >= 0) {
            out[n++] = (char)(hex_value(p[1]) * 16 + hex_value(p[2]));
            p += 2;
        } else {
            out[n++] = *p;
        }
    }
    out[n] = '\0';
    /* /c:/x on Windows is c:/x */
    if (n >= 3 && out[0] == '/' && out[2] == ':' &&
        ((out[1] >= 'a' && out[1] <= 'z') || (out[1] >= 'A' && out[1] <= 'Z'))) {
        memmove(out, out + 1, n);
    }
    return out;
}

static bool uri_safe(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '.' || c == '_' || c == '~' || c == '/' || c == ':';
}

char *plum_lsp_path_to_uri(const char *path) {
    size_t len = strlen(path);
    char *out = plum_alloc(plum_add_size(plum_mul_size(len, 3), 16));
    size_t n = 0;
    memcpy(out, "file://", 7);
    n = 7;
    if (path[0] != '/') {
        out[n++] = '/'; /* c:/x is file:///c:/x */
    }
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c == '\\') {
            out[n++] = '/';
        } else if (uri_safe(c)) {
            out[n++] = (char)c;
        } else {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = '\0';
    return out;
}
