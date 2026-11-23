/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
#include "lsp/json.h"

#include "util/alloc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

#define MAX_NESTING 128

typedef struct Reader {
    PlumArena *arena;
    const char *p;
    const char *end;
    int depth;
} Reader;

static void skip_space(Reader *r) {
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')) {
        r->p++;
    }
}

static PlumJson *new_value(Reader *r, PlumJsonKind kind) {
    PlumJson *v = plum_arena_alloc_zeroed(r->arena, sizeof *v);
    v->kind = kind;
    return v;
}

static bool literal(Reader *r, const char *word) {
    size_t n = strlen(word);
    if ((size_t)(r->end - r->p) < n || memcmp(r->p, word, n) != 0) {
        return false;
    }
    r->p += n;
    return true;
}

static int hex_digit(char c) {
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

/* Four hex digits after \u; -1 if they aren't. */
static long hex4(Reader *r) {
    if (r->end - r->p < 4) {
        return -1;
    }
    long v = 0;
    for (int i = 0; i < 4; i++) {
        int d = hex_digit(r->p[i]);
        if (d < 0) {
            return -1;
        }
        v = v * 16 + d;
    }
    r->p += 4;
    return v;
}

typedef PLUM_ARRAY(char) CharBuf;

static void put_utf8(CharBuf *b, unsigned long c) {
    if (c < 0x80) {
        plum_array_push(b, (char)c);
    } else if (c < 0x800) {
        plum_array_push(b, (char)(0xC0 | (c >> 6)));
        plum_array_push(b, (char)(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        plum_array_push(b, (char)(0xE0 | (c >> 12)));
        plum_array_push(b, (char)(0x80 | ((c >> 6) & 0x3F)));
        plum_array_push(b, (char)(0x80 | (c & 0x3F)));
    } else {
        plum_array_push(b, (char)(0xF0 | (c >> 18)));
        plum_array_push(b, (char)(0x80 | ((c >> 12) & 0x3F)));
        plum_array_push(b, (char)(0x80 | ((c >> 6) & 0x3F)));
        plum_array_push(b, (char)(0x80 | (c & 0x3F)));
    }
}

/* A string, after its opening quote; its text goes into the arena. */
static bool read_string(Reader *r, const char **chars, size_t *len) {
    CharBuf b = {0};
    while (r->p < r->end && *r->p != '"') {
        unsigned char c = (unsigned char)*r->p;
        if (c < 0x20) {
            plum_array_free(&b);
            return false; /* raw control characters aren't allowed */
        }
        if (c != '\\') {
            plum_array_push(&b, (char)c);
            r->p++;
            continue;
        }
        r->p++;
        if (r->p >= r->end) {
            break;
        }
        char e = *r->p++;
        switch (e) {
        case '"':
        case '\\':
        case '/':
            plum_array_push(&b, e);
            break;
        case 'b':
            plum_array_push(&b, '\b');
            break;
        case 'f':
            plum_array_push(&b, '\f');
            break;
        case 'n':
            plum_array_push(&b, '\n');
            break;
        case 'r':
            plum_array_push(&b, '\r');
            break;
        case 't':
            plum_array_push(&b, '\t');
            break;
        case 'u': {
            long u = hex4(r);
            if (u < 0) {
                plum_array_free(&b);
                return false;
            }
            unsigned long code = (unsigned long)u;
            if (code >= 0xD800 && code <= 0xDBFF) {
                /* a high surrogate: joined with the low one after it */
                const char *save = r->p;
                long lo = -1;
                if (r->end - r->p >= 6 && r->p[0] == '\\' && r->p[1] == 'u') {
                    r->p += 2;
                    lo = hex4(r);
                }
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    code = 0x10000 + ((code - 0xD800) << 10) + ((unsigned long)lo - 0xDC00);
                } else {
                    r->p = save;
                    code = 0xFFFD;
                }
            } else if (code >= 0xDC00 && code <= 0xDFFF) {
                code = 0xFFFD;
            }
            put_utf8(&b, code);
            break;
        }
        default:
            plum_array_free(&b);
            return false;
        }
    }
    if (r->p >= r->end) {
        plum_array_free(&b);
        return false; /* no closing quote */
    }
    r->p++;
    *len = b.len;
    *chars = plum_arena_strndup(r->arena, b.items != NULL ? b.items : "", b.len);
    plum_array_free(&b);
    return true;
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static PlumJson *read_number(Reader *r) {
    const char *start = r->p;
    if (r->p < r->end && *r->p == '-') {
        r->p++;
    }
    if (r->p >= r->end || !is_digit(*r->p)) {
        return NULL;
    }
    if (*r->p == '0') {
        r->p++;
    } else {
        while (r->p < r->end && is_digit(*r->p)) {
            r->p++;
        }
    }
    if (r->p < r->end && *r->p == '.') {
        r->p++;
        if (r->p >= r->end || !is_digit(*r->p)) {
            return NULL;
        }
        while (r->p < r->end && is_digit(*r->p)) {
            r->p++;
        }
    }
    if (r->p < r->end && (*r->p == 'e' || *r->p == 'E')) {
        r->p++;
        if (r->p < r->end && (*r->p == '+' || *r->p == '-')) {
            r->p++;
        }
        if (r->p >= r->end || !is_digit(*r->p)) {
            return NULL;
        }
        while (r->p < r->end && is_digit(*r->p)) {
            r->p++;
        }
    }
    char *copy = plum_arena_strndup(r->arena, start, (size_t)(r->p - start));
    PlumJson *v = new_value(r, PLUM_JSON_NUMBER);
    v->as.n = strtod(copy, NULL);
    return v;
}

static PlumJson *read_value(Reader *r);

static PlumJson *read_array(Reader *r) {
    PLUM_ARRAY(PlumJson *) items = {0};
    skip_space(r);
    if (r->p < r->end && *r->p == ']') {
        r->p++;
        return new_value(r, PLUM_JSON_ARRAY);
    }
    for (;;) {
        PlumJson *item = read_value(r);
        if (item == NULL) {
            plum_array_free(&items);
            return NULL;
        }
        plum_array_push(&items, item);
        skip_space(r);
        if (r->p < r->end && *r->p == ',') {
            r->p++;
            continue;
        }
        if (r->p < r->end && *r->p == ']') {
            r->p++;
            break;
        }
        plum_array_free(&items);
        return NULL;
    }
    PlumJson *v = new_value(r, PLUM_JSON_ARRAY);
    v->as.arr.items = plum_arena_alloc(r->arena, items.len * sizeof(PlumJson *));
    memcpy(v->as.arr.items, items.items, items.len * sizeof(PlumJson *));
    v->as.arr.count = items.len;
    plum_array_free(&items);
    return v;
}

static PlumJson *read_object(Reader *r) {
    PLUM_ARRAY(PlumJsonMember) members = {0};
    skip_space(r);
    if (r->p < r->end && *r->p == '}') {
        r->p++;
        return new_value(r, PLUM_JSON_OBJECT);
    }
    for (;;) {
        skip_space(r);
        PlumJsonMember m;
        size_t len = 0;
        if (r->p >= r->end || *r->p != '"') {
            plum_array_free(&members);
            return NULL;
        }
        r->p++;
        if (!read_string(r, &m.key, &len)) {
            plum_array_free(&members);
            return NULL;
        }
        skip_space(r);
        if (r->p >= r->end || *r->p != ':') {
            plum_array_free(&members);
            return NULL;
        }
        r->p++;
        m.value = read_value(r);
        if (m.value == NULL) {
            plum_array_free(&members);
            return NULL;
        }
        plum_array_push(&members, m);
        skip_space(r);
        if (r->p < r->end && *r->p == ',') {
            r->p++;
            continue;
        }
        if (r->p < r->end && *r->p == '}') {
            r->p++;
            break;
        }
        plum_array_free(&members);
        return NULL;
    }
    PlumJson *v = new_value(r, PLUM_JSON_OBJECT);
    v->as.obj.members = plum_arena_alloc(r->arena, members.len * sizeof(PlumJsonMember));
    memcpy(v->as.obj.members, members.items, members.len * sizeof(PlumJsonMember));
    v->as.obj.count = members.len;
    plum_array_free(&members);
    return v;
}

static PlumJson *read_value(Reader *r) {
    skip_space(r);
    if (r->p >= r->end) {
        return NULL;
    }
    char c = *r->p;
    PlumJson *v = NULL;
    if (c == '{' || c == '[') {
        if (r->depth >= MAX_NESTING) {
            return NULL;
        }
        r->depth++;
        r->p++;
        v = c == '{' ? read_object(r) : read_array(r);
        r->depth--;
    } else if (c == '"') {
        r->p++;
        v = new_value(r, PLUM_JSON_STRING);
        if (!read_string(r, &v->as.str.chars, &v->as.str.len)) {
            return NULL;
        }
    } else if (literal(r, "true")) {
        v = new_value(r, PLUM_JSON_BOOL);
        v->as.b = true;
    } else if (literal(r, "false")) {
        v = new_value(r, PLUM_JSON_BOOL);
    } else if (literal(r, "null")) {
        v = new_value(r, PLUM_JSON_NULL);
    } else {
        v = read_number(r);
    }
    return v;
}

PlumJson *plum_json_parse(PlumArena *arena, const char *text, size_t len) {
    Reader r = {arena, text, text + len, 0};
    PlumJson *v = read_value(&r);
    skip_space(&r);
    return v != NULL && r.p == r.end ? v : NULL;
}

const PlumJson *plum_json_get(const PlumJson *obj, const char *key) {
    if (obj == NULL || obj->kind != PLUM_JSON_OBJECT) {
        return NULL;
    }
    for (size_t i = 0; i < obj->as.obj.count; i++) {
        if (strcmp(obj->as.obj.members[i].key, key) == 0) {
            return obj->as.obj.members[i].value;
        }
    }
    return NULL;
}

const char *plum_json_str(const PlumJson *obj, const char *key, size_t *len) {
    const PlumJson *v = plum_json_get(obj, key);
    if (v == NULL || v->kind != PLUM_JSON_STRING) {
        return NULL;
    }
    if (len != NULL) {
        *len = v->as.str.len;
    }
    return v->as.str.chars;
}

long long plum_json_int(const PlumJson *obj, const char *key, long long fallback) {
    const PlumJson *v = plum_json_get(obj, key);
    if (v == NULL || v->kind != PLUM_JSON_NUMBER || !(v->as.n >= -9e15 && v->as.n <= 9e15)) {
        return fallback;
    }
    return (long long)v->as.n;
}

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

void plum_jw_init(PlumJsonWriter *w) {
    memset(w, 0, sizeof *w);
    w->first[0] = true;
}

void plum_jw_free(PlumJsonWriter *w) {
    plum_array_free(&w->buf);
}

const char *plum_jw_text(PlumJsonWriter *w, size_t *len) {
    plum_array_reserve(&w->buf, 1);
    w->buf.items[w->buf.len] = '\0';
    *len = w->buf.len;
    return w->buf.items;
}

static void put(PlumJsonWriter *w, const char *s, size_t n) {
    plum_array_reserve(&w->buf, n);
    memcpy(w->buf.items + w->buf.len, s, n);
    w->buf.len += n;
}

static void puts_(PlumJsonWriter *w, const char *s) {
    put(w, s, strlen(s));
}

/* Before a value or a key: the comma that separates it from the last. */
static void separate(PlumJsonWriter *w) {
    if (w->after_key) {
        w->after_key = false;
        return;
    }
    if (!w->first[w->depth]) {
        put(w, ",", 1);
    }
    w->first[w->depth] = false;
}

static void open_(PlumJsonWriter *w, const char *bracket) {
    separate(w);
    puts_(w, bracket);
    if (w->depth + 1 < PLUM_JSON_DEPTH) {
        w->depth++;
    }
    w->first[w->depth] = true;
}

static void close_(PlumJsonWriter *w, const char *bracket) {
    puts_(w, bracket);
    if (w->depth > 0) {
        w->depth--;
    }
}

void plum_jw_object(PlumJsonWriter *w) {
    open_(w, "{");
}

void plum_jw_object_end(PlumJsonWriter *w) {
    close_(w, "}");
}

void plum_jw_array(PlumJsonWriter *w) {
    open_(w, "[");
}

void plum_jw_array_end(PlumJsonWriter *w) {
    close_(w, "]");
}

/* Length of the valid UTF-8 character at s[0..n), or 0 if it isn't one. */
static size_t utf8_char(const unsigned char *s, size_t n) {
    size_t len = s[0] < 0x80             ? 1
                 : (s[0] & 0xE0) == 0xC0 ? 2
                 : (s[0] & 0xF0) == 0xE0 ? 3
                 : (s[0] & 0xF8) == 0xF0 ? 4
                                         : 0;
    if (len == 0 || len > n) {
        return 0;
    }
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            return 0;
        }
    }
    /* overlong forms and surrogates aren't valid either */
    if ((len == 2 && s[0] < 0xC2) || (len == 3 && s[0] == 0xE0 && s[1] < 0xA0) ||
        (len == 3 && s[0] == 0xED && s[1] >= 0xA0) || (len == 4 && s[0] == 0xF0 && s[1] < 0x90) ||
        (len == 4 && (s[0] > 0xF4 || (s[0] == 0xF4 && s[1] >= 0x90)))) {
        return 0;
    }
    return len;
}

static void write_string(PlumJsonWriter *w, const char *s, size_t len) {
    const unsigned char *u = (const unsigned char *)s;
    put(w, "\"", 1);
    size_t i = 0;
    while (i < len) {
        unsigned char c = u[i];
        if (c == '"' || c == '\\') {
            char esc[2] = {'\\', (char)c};
            put(w, esc, 2);
            i++;
        } else if (c == '\n') {
            puts_(w, "\\n");
            i++;
        } else if (c == '\r') {
            puts_(w, "\\r");
            i++;
        } else if (c == '\t') {
            puts_(w, "\\t");
            i++;
        } else if (c < 0x20 || c == 0x7F) {
            char esc[8];
            snprintf(esc, sizeof esc, "\\u%04x", c);
            puts_(w, esc);
            i++;
        } else if (c < 0x80) {
            put(w, s + i, 1);
            i++;
        } else {
            size_t n = utf8_char(u + i, len - i);
            if (n == 0) {
                puts_(w, "\xEF\xBF\xBD"); /* U+FFFD for each invalid byte */
                i++;
            } else {
                put(w, s + i, n);
                i += n;
            }
        }
    }
    put(w, "\"", 1);
}

void plum_jw_key(PlumJsonWriter *w, const char *key) {
    separate(w);
    write_string(w, key, strlen(key));
    put(w, ":", 1);
    w->after_key = true;
}

void plum_jw_string(PlumJsonWriter *w, const char *s) {
    plum_jw_stringn(w, s, strlen(s));
}

void plum_jw_stringn(PlumJsonWriter *w, const char *s, size_t len) {
    separate(w);
    write_string(w, s, len);
}

void plum_jw_int(PlumJsonWriter *w, long long n) {
    char num[32];
    separate(w);
    snprintf(num, sizeof num, "%lld", n);
    puts_(w, num);
}

void plum_jw_bool(PlumJsonWriter *w, bool b) {
    separate(w);
    puts_(w, b ? "true" : "false");
}

void plum_jw_null(PlumJsonWriter *w) {
    separate(w);
    puts_(w, "null");
}

void plum_jw_value(PlumJsonWriter *w, const PlumJson *v) {
    if (v == NULL) {
        plum_jw_null(w);
        return;
    }
    switch (v->kind) {
    case PLUM_JSON_NULL:
        plum_jw_null(w);
        break;
    case PLUM_JSON_BOOL:
        plum_jw_bool(w, v->as.b);
        break;
    case PLUM_JSON_NUMBER:
        if (v->as.n == floor(v->as.n) && fabs(v->as.n) < 9e15) {
            plum_jw_int(w, (long long)v->as.n);
        } else {
            char num[40];
            separate(w);
            snprintf(num, sizeof num, "%.17g", isfinite(v->as.n) ? v->as.n : 0.0);
            puts_(w, num);
        }
        break;
    case PLUM_JSON_STRING:
        plum_jw_stringn(w, v->as.str.chars, v->as.str.len);
        break;
    case PLUM_JSON_ARRAY:
        plum_jw_array(w);
        for (size_t i = 0; i < v->as.arr.count; i++) {
            plum_jw_value(w, v->as.arr.items[i]);
        }
        plum_jw_array_end(w);
        break;
    case PLUM_JSON_OBJECT:
        plum_jw_object(w);
        for (size_t i = 0; i < v->as.obj.count; i++) {
            plum_jw_key(w, v->as.obj.members[i].key);
            plum_jw_value(w, v->as.obj.members[i].value);
        }
        plum_jw_object_end(w);
        break;
    }
}
