/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
#include "orchard/toml.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Is s[0..len) UTF-8 (no overlong forms, surrogates or code points past
 * U+10FFFF)? With `nul`, a zero byte is allowed. */
static bool utf8_check(const char *s, size_t len, bool nul) {
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < len) {
        unsigned c = p[i];
        size_t extra;
        uint32_t cp;
        if (c == 0 && !nul) {
            return false;
        } else if (c < 0x80) {
            i++;
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            extra = 1;
            cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2;
            cp = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (len - i <= extra) {
            return false;
        }
        for (size_t k = 1; k <= extra; k++) {
            if ((p[i + k] & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        /* no overlong forms, surrogates, or code points past U+10FFFF */
        if ((extra == 2 && cp < 0x800) || (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

bool plum_utf8_valid(const char *s, size_t len) {
    return utf8_check(s, len, false);
}

/* How a table or array of a full document came to be. A table defined
 * one way can't be defined again, nor added to another way. */
enum {
    MADE_IMPLICIT = 1, /* as the parent in a [header]: [a] may still define it */
    MADE_HEADER = 2,   /* by a [header] or [[header]] */
    MADE_DOTTED = 4,   /* by a dotted key: more dotted keys may add to it */
    MADE_INLINE = 8,   /* an inline table: closed once its } is read */
    MADE_AOT = 16,     /* an array made by [[header]], not a value */
};

/* Arrays and inline tables nest at most this deep. */
#define MAX_DEPTH 256

typedef struct Parser {
    PlumToml *doc;
    const PlumSource *src;
    PlumDiagList *diags;
    size_t pos;
    uint32_t line;
    uint32_t column;
    bool full;                /* all of TOML 1.0, not plum.toml's subset */
    const char *table;        /* the current table */
    PlumTomlTable *table_rec; /* NULL at the top level */
    size_t table_index;
    PlumTomlNode *current; /* full: the table key = value lines go into */
    bool failed;
} Parser;

static char peek(const Parser *p) {
    return p->pos < p->src->length ? p->src->text[p->pos] : '\0';
}

static char peek_at(const Parser *p, size_t ahead) {
    return p->pos + ahead < p->src->length ? p->src->text[p->pos + ahead] : '\0';
}

static bool at_end(const Parser *p) {
    return p->pos >= p->src->length;
}

static void advance(Parser *p) {
    if (at_end(p)) {
        return;
    }
    unsigned char c = (unsigned char)p->src->text[p->pos++];
    if (c == '\n') {
        p->line++;
        p->column = 1;
    } else if ((c & 0xC0) != 0x80) {
        p->column++; /* columns count characters, not bytes */
    }
}

static PlumSpan here(const Parser *p) {
    PlumSpan s = {(uint32_t)p->pos, 1, p->line, p->column};
    return s;
}

PLUM_PRINTF(3, 4)
static void fail(Parser *p, PlumSpan span, const char *fmt, ...);

static void fail(Parser *p, PlumSpan span, const char *fmt, ...) {
    char message[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof message, fmt, args);
    va_end(args);
    if (span.offset >= p->src->length) {
        span.length = 0;
    }
    plum_diag_error(p->diags, span, "E0901", "%s", message);
    p->failed = true;
}

/* A name defined twice (E0902). */
#define fail_twice(p, span, ...)                                   \
    do {                                                           \
        plum_diag_error((p)->diags, (span), "E0902", __VA_ARGS__); \
        (p)->failed = true;                                        \
    } while (0)

static void skip_spaces(Parser *p) {
    while (peek(p) == ' ' || peek(p) == '\t') {
        advance(p);
    }
}

/* A comment, at its '#': up to the line's end. False (after an error) if
 * it holds a control character. */
static bool skip_comment(Parser *p) {
    while (!at_end(p) && peek(p) != '\n') {
        unsigned char c = (unsigned char)peek(p);
        if ((c < 0x20 && c != '\t' && c != '\r') || (p->full && c == 0x7F)) {
            fail(p, here(p), "a control character in a comment");
            return false;
        }
        if (c == '\r' && p->full && peek_at(p, 1) != '\n') {
            fail(p, here(p), "a control character in a comment");
            return false;
        }
        advance(p);
    }
    return true;
}

/* Spaces, comments and line breaks (inside an array). */
static void skip_blank(Parser *p) {
    for (;;) {
        char c = peek(p);
        if (c == ' ' || c == '\t' || c == '\n' ||
            (c == '\r' && p->pos + 1 < p->src->length && p->src->text[p->pos + 1] == '\n')) {
            advance(p);
        } else if (c == '#') {
            if (!p->full) {
                while (!at_end(p) && peek(p) != '\n') {
                    advance(p);
                }
            } else if (!skip_comment(p)) {
                return;
            }
        } else {
            return;
        }
    }
}

/* After a value or header: spaces, maybe a comment, then the line's end. */
static bool end_of_line(Parser *p) {
    skip_spaces(p);
    if (peek(p) == '#' && !skip_comment(p)) {
        return false;
    }
    if (peek(p) == '\r' && p->pos + 1 < p->src->length && p->src->text[p->pos + 1] == '\n') {
        advance(p);
    }
    if (at_end(p) || peek(p) == '\n') {
        advance(p);
        return true;
    }
    fail(p, here(p), "unexpected '%c' after the value; put one key = value on each line", peek(p));
    return false;
}

static bool is_bare(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
}

typedef PLUM_ARRAY(char) CharBuf;

static void put_utf8(CharBuf *out, uint32_t cp) {
    if (cp < 0x80) {
        plum_array_push(out, (char)cp);
    } else if (cp < 0x800) {
        plum_array_push(out, (char)(0xC0 | (cp >> 6)));
        plum_array_push(out, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        plum_array_push(out, (char)(0xE0 | (cp >> 12)));
        plum_array_push(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
        plum_array_push(out, (char)(0x80 | (cp & 0x3F)));
    } else {
        plum_array_push(out, (char)(0xF0 | (cp >> 18)));
        plum_array_push(out, (char)(0x80 | ((cp >> 12) & 0x3F)));
        plum_array_push(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
        plum_array_push(out, (char)(0x80 | (cp & 0x3F)));
    }
}

/* An escape, at its backslash, into `out`. False after an error. */
static bool parse_escape(Parser *p, CharBuf *out) {
    PlumSpan esc = here(p);
    advance(p);
    char e = peek(p);
    advance(p);
    switch (e) {
    case '"':
    case '\\':
        plum_array_push(out, e);
        return true;
    case 'n':
        plum_array_push(out, '\n');
        return true;
    case 't':
        plum_array_push(out, '\t');
        return true;
    case 'r':
        plum_array_push(out, '\r');
        return true;
    case 'b':
        plum_array_push(out, '\b');
        return true;
    case 'f':
        plum_array_push(out, '\f');
        return true;
    case 'u':
    case 'U': {
        int digits = e == 'u' ? 4 : 8;
        uint32_t cp = 0;
        for (int k = 0; k < digits; k++) {
            char h = peek(p);
            int v = h >= '0' && h <= '9'   ? h - '0'
                    : h >= 'a' && h <= 'f' ? h - 'a' + 10
                    : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                           : -1;
            if (v < 0) {
                fail(p, esc, "\\%c needs %d hex digits", e, digits);
                return false;
            }
            cp = cp * 16 + (uint32_t)v;
            advance(p);
        }
        if ((cp == 0 && !p->full) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            fail(p, esc, "\\%c names no character a string can hold", e);
            return false;
        }
        put_utf8(out, cp);
        return true;
    }
    default:
        fail(p, esc, "an unknown escape; use \\\\ for a backslash");
        return false;
    }
}

/* A """multi-line""" or '''multi-line''' string, at its first quote. */
static const char *parse_multiline(Parser *p, size_t *len_out) {
    PlumSpan start = here(p);
    char quote = peek(p);
    advance(p);
    advance(p);
    advance(p);
    /* a line break right after the opening quotes is left out */
    if (peek(p) == '\n') {
        advance(p);
    } else if (peek(p) == '\r' && peek_at(p, 1) == '\n') {
        advance(p);
        advance(p);
    }
    CharBuf out = {0};
    for (;;) {
        if (at_end(p)) {
            fail(p, start, "this string has no closing %c%c%c", quote, quote, quote);
            plum_array_free(&out);
            return NULL;
        }
        unsigned char c = (unsigned char)peek(p);
        if (c == (unsigned char)quote && peek_at(p, 1) == quote && peek_at(p, 2) == quote) {
            /* up to two more quotes just before the closing three are text */
            int extra = 0;
            while (extra < 3 && peek_at(p, (size_t)(3 + extra)) == quote) {
                extra++;
            }
            if (extra > 2) {
                fail(p, here(p), "too many %c in a row; escape one", quote);
                plum_array_free(&out);
                return NULL;
            }
            for (int k = 0; k < extra; k++) {
                plum_array_push(&out, quote);
                advance(p);
            }
            advance(p);
            advance(p);
            advance(p);
            break;
        }
        if (c == '\r' && peek_at(p, 1) == '\n') {
            advance(p);
            continue; /* \r\n is a line break, kept as \n */
        }
        if ((c < 0x20 && c != '\t' && c != '\n') || c == 0x7F) {
            fail(p, here(p), "a control character in a string; write it as an escape");
            plum_array_free(&out);
            return NULL;
        }
        if (c == '\\' && quote == '"') {
            /* a backslash at a line's end takes the line break and the
             * spaces and breaks after it */
            size_t k = 1;
            while (peek_at(p, k) == ' ' || peek_at(p, k) == '\t') {
                k++;
            }
            if (peek_at(p, k) == '\n' || (peek_at(p, k) == '\r' && peek_at(p, k + 1) == '\n')) {
                advance(p);
                for (;;) {
                    char w = peek(p);
                    if (w == ' ' || w == '\t' || w == '\n' ||
                        (w == '\r' && peek_at(p, 1) == '\n')) {
                        advance(p);
                    } else {
                        break;
                    }
                }
                continue;
            }
            if (!parse_escape(p, &out)) {
                plum_array_free(&out);
                return NULL;
            }
            continue;
        }
        plum_array_push(&out, (char)c);
        advance(p);
    }
    *len_out = out.len;
    char *s = plum_arena_strndup(&p->doc->arena, out.items != NULL ? out.items : "", out.len);
    plum_array_free(&out);
    return s;
}

/* A "basic" or 'literal' string, at its opening quote; its length into
 * *len_out. NULL after an error. */
static const char *parse_string(Parser *p, size_t *len_out) {
    PlumSpan start = here(p);
    char quote = peek(p);
    if (peek_at(p, 1) == quote && peek_at(p, 2) == quote) {
        if (p->full) {
            return parse_multiline(p, len_out);
        }
        fail(p, start, "multi-line strings aren't supported here; write the text on one line");
        return NULL;
    }
    advance(p);
    CharBuf out = {0};
    for (;;) {
        if (at_end(p) || peek(p) == '\n' || peek(p) == '\r') {
            fail(p, start, "this string has no closing %c on its line", quote);
            plum_array_free(&out);
            return NULL;
        }
        unsigned char c = (unsigned char)peek(p);
        if (c == (unsigned char)quote) {
            advance(p);
            break;
        }
        if ((c < 0x20 && c != '\t') || c == 0x7F) {
            fail(p, here(p), "a control character in a string; write it as an escape");
            plum_array_free(&out);
            return NULL;
        }
        if (c == '\\' && quote == '"') {
            if (!parse_escape(p, &out)) {
                plum_array_free(&out);
                return NULL;
            }
            continue;
        }
        plum_array_push(&out, (char)c);
        advance(p);
    }
    if (!utf8_check(out.items != NULL ? out.items : "", out.len, p->full)) {
        fail(p, start, "this string isn't valid UTF-8");
        plum_array_free(&out);
        return NULL;
    }
    *len_out = out.len;
    char *s = plum_arena_strndup(&p->doc->arena, out.items != NULL ? out.items : "", out.len);
    plum_array_free(&out);
    return s;
}

/* A bare or quoted key, its length into *len_out. NULL after an error. */
static const char *parse_key(Parser *p, const char *what, size_t *len_out) {
    if (peek(p) == '"' || peek(p) == '\'') {
        if (p->full && peek_at(p, 1) == peek(p) && peek_at(p, 2) == peek(p)) {
            fail(p, here(p), "a %s can't be a multi-line string", what);
            return NULL;
        }
        const char *k = parse_string(p, len_out);
        if (k != NULL && k[0] == '\0' && !p->full) {
            fail(p, here(p), "an empty %s", what);
            return NULL;
        }
        return k;
    }
    size_t start = p->pos;
    while (is_bare(peek(p))) {
        advance(p);
    }
    if (p->pos == start) {
        fail(p, here(p), "expected a %s (letters, digits, '_' and '-'), found '%c'", what,
             at_end(p) ? ' ' : peek(p));
        return NULL;
    }
    *len_out = p->pos - start;
    return plum_arena_strndup(&p->doc->arena, p->src->text + start, p->pos - start);
}

static PlumSpan span_from(const Parser *p, PlumSpan start) {
    start.length = (uint32_t)(p->pos - start.offset);
    return start;
}

static char *join(Parser *p, const char *table, const char *key) {
    size_t a = strlen(table), b = strlen(key);
    char *s = plum_arena_alloc(&p->doc->arena, a + b + 2);
    memcpy(s, table, a);
    size_t at = a;
    if (a > 0) {
        s[at++] = '.';
    }
    memcpy(s + at, key, b + 1);
    return s;
}

/* ------------------------------------------------------------------ */
/* plum.toml's subset                                                  */
/* ------------------------------------------------------------------ */

/* [table] or [table.sub] */
static void parse_header(Parser *p) {
    PlumSpan start = here(p);
    advance(p);
    if (peek(p) == '[') {
        fail(p, start, "arrays of tables ([[...]]) aren't supported here");
        return;
    }
    const char *name = "";
    for (;;) {
        skip_spaces(p);
        size_t part_len;
        const char *part = parse_key(p, "table name", &part_len);
        if (part == NULL) {
            return;
        }
        name = join(p, name, part);
        skip_spaces(p);
        if (peek(p) == '.') {
            advance(p);
            continue;
        }
        if (peek(p) == ']') {
            advance(p);
            break;
        }
        fail(p, here(p), "expected ']' to close the table name");
        return;
    }
    PlumSpan span = span_from(p, start);
    if (!end_of_line(p)) {
        return;
    }
    for (size_t i = 0; i < p->doc->tables.len; i++) {
        if (strcmp(p->doc->tables.items[i].name, name) == 0) {
            fail_twice(p, span, "the table [%s] is already declared on line %u; keep one", name,
                       (unsigned)p->doc->tables.items[i].span.line);
            return;
        }
    }
    for (size_t i = 0; i < p->doc->values.len; i++) {
        const PlumTomlValue *v = &p->doc->values.items[i];
        if (strcmp(join(p, v->table, v->key), name) == 0) {
            fail_twice(p, span, "[%s] is already a key, on line %u; a name can't be both", name,
                       (unsigned)v->key_span.line);
            return;
        }
    }
    PlumTomlTable t = {name, span, span.line};
    plum_array_push(&p->doc->tables, t);
    p->table = name;
    p->table_index = p->doc->tables.len - 1;
    p->table_rec = &p->doc->tables.items[p->table_index];
}

/* key = value */
static void parse_pair(Parser *p) {
    PlumSpan key_start = here(p);
    size_t key_len;
    const char *key = parse_key(p, "key", &key_len);
    if (key == NULL) {
        return;
    }
    PlumSpan key_span = span_from(p, key_start);
    skip_spaces(p);
    if (peek(p) == '.') {
        fail(p, here(p), "dotted keys aren't supported here; use a [table]");
        return;
    }
    if (peek(p) != '=') {
        fail(p, here(p), "expected '=' after the key '%s'", key);
        return;
    }
    advance(p);
    skip_spaces(p);
    PlumTomlValue v;
    memset(&v, 0, sizeof v);
    v.table = p->table;
    v.key = key;
    v.key_span = key_span;
    PlumSpan value_start = here(p);
    char c = peek(p);
    size_t len = 0;
    if (c == '"' || c == '\'') {
        v.kind = PLUM_TOML_STRING;
        v.str = parse_string(p, &len);
        if (v.str == NULL) {
            return;
        }
    } else if (c == '[') {
        v.kind = PLUM_TOML_ARRAY;
        advance(p);
        PLUM_ARRAY(const char *) items = {0};
        for (;;) {
            skip_blank(p);
            if (peek(p) == ']') {
                advance(p);
                break;
            }
            if (peek(p) != '"' && peek(p) != '\'') {
                fail(p, here(p),
                     at_end(p) ? "this list has no closing ']'"
                               : "a list here holds strings only, in quotes");
                plum_array_free(&items);
                return;
            }
            const char *s = parse_string(p, &len);
            if (s == NULL) {
                plum_array_free(&items);
                return;
            }
            plum_array_push(&items, s);
            skip_blank(p);
            if (peek(p) == ',') {
                advance(p);
            } else if (peek(p) != ']') {
                fail(p, here(p), "expected ',' or ']' in the list");
                plum_array_free(&items);
                return;
            }
        }
        v.count = items.len;
        v.items =
            plum_arena_alloc(&p->doc->arena, (items.len > 0 ? items.len : 1) * sizeof(char *));
        for (size_t i = 0; i < items.len; i++) {
            v.items[i] = items.items[i];
        }
        plum_array_free(&items);
    } else if (c == '{') {
        fail(p, value_start, "inline tables aren't supported here; use a [table]");
        return;
    } else if (at_end(p) || c == '\n' || c == '\r' || c == '#') {
        fail(p, value_start, "the key '%s' has no value; write %s = \"...\"", key, key);
        return;
    } else {
        fail(p, value_start,
             "only strings (in quotes) and lists of strings are allowed here; "
             "write \"%c...\"",
             c);
        return;
    }
    v.value_span = span_from(p, value_start);
    v.last_line = p->line;
    if (!end_of_line(p)) {
        return;
    }
    for (size_t i = 0; i < p->doc->values.len; i++) {
        const PlumTomlValue *o = &p->doc->values.items[i];
        if (strcmp(o->table, v.table) == 0 && strcmp(o->key, v.key) == 0) {
            fail_twice(p, key_span, "'%s' is already set on line %u; keep one", key,
                       (unsigned)o->key_span.line);
            return;
        }
    }
    const char *full = join(p, v.table, v.key);
    if (plum_toml_table(p->doc, full) != NULL) {
        fail_twice(p, key_span, "'%s' is already a [table]; a name can't be both", full);
        return;
    }
    plum_array_push(&p->doc->values, v);
    if (p->table_rec != NULL) {
        p->doc->tables.items[p->table_index].last_line = v.last_line;
    }
}

/* ------------------------------------------------------------------ */
/* All of TOML 1.0                                                     */
/* ------------------------------------------------------------------ */

static PlumTomlNode *new_node(Parser *p, PlumTomlNodeKind kind, PlumSpan span) {
    PlumTomlNode *node = plum_arena_alloc_zeroed(&p->doc->arena, sizeof *node);
    node->kind = kind;
    node->span = span;
    return node;
}

/* A key of a full document: it may hold \u0000. */
typedef struct Key {
    const char *s;
    size_t len;
} Key;

static const Key no_key = {NULL, 0};

/* Add `child` to an array (no key) or a table. */
static void node_add(Parser *p, PlumTomlNode *parent, Key key, PlumTomlNode *child) {
    if (parent->count == parent->cap) {
        size_t cap = parent->cap > 0 ? parent->cap * 2 : 4;
        PlumTomlNode **items = plum_arena_alloc(&p->doc->arena, cap * sizeof *items);
        if (parent->count > 0) {
            memcpy(items, parent->items, parent->count * sizeof *items);
        }
        parent->items = items;
        if (parent->kind == PLUM_TOML_NODE_TABLE) {
            const char **keys = plum_arena_alloc(&p->doc->arena, cap * sizeof *keys);
            if (parent->count > 0) {
                memcpy(keys, parent->keys, parent->count * sizeof *keys);
            }
            parent->keys = keys;
            size_t *lens = plum_arena_alloc(&p->doc->arena, cap * sizeof *lens);
            if (parent->count > 0) {
                memcpy(lens, parent->key_lens, parent->count * sizeof *lens);
            }
            parent->key_lens = lens;
        }
        parent->cap = cap;
    }
    if (parent->kind == PLUM_TOML_NODE_TABLE) {
        parent->keys[parent->count] = key.s;
        parent->key_lens[parent->count] = key.len;
    }
    parent->items[parent->count++] = child;
}

static PlumTomlNode *node_find(const PlumTomlNode *table, Key key) {
    for (size_t i = 0; i < table->count; i++) {
        if (table->key_lens[i] == key.len && memcmp(table->keys[i], key.s, key.len) == 0) {
            return table->items[i];
        }
    }
    return NULL;
}

/* A key, dotted or not: its parts into `parts`. False after an error. */
typedef PLUM_ARRAY(Key) Parts;

static bool parse_dotted_key(Parser *p, Parts *parts, const char *what) {
    for (;;) {
        skip_spaces(p);
        Key part;
        part.s = parse_key(p, what, &part.len);
        if (part.s == NULL) {
            return false;
        }
        plum_array_push(parts, part);
        skip_spaces(p);
        if (peek(p) != '.') {
            return true;
        }
        advance(p);
    }
}

static const char *describe(const PlumTomlNode *node) {
    switch (node->kind) {
    case PLUM_TOML_NODE_TABLE:
        return (node->flags & MADE_INLINE) != 0 ? "an inline table" : "a table";
    case PLUM_TOML_NODE_ARRAY:
        return (node->flags & MADE_AOT) != 0 ? "an array of tables" : "an array";
    default:
        return "a value";
    }
}

static PlumTomlNode *parse_value(Parser *p, int depth);

/* ---- Numbers, dates and times -------------------------------------- */

static bool all_digits(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

static int two(const char *s) {
    return (s[0] - '0') * 10 + (s[1] - '0');
}

/* YYYY-MM-DD at s, a real day. */
static bool date_ok(const char *s) {
    int year = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + two(s + 2);
    int month = two(s + 5), day = two(s + 8);
    static const int days[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12 || day < 1 || day > days[month - 1]) {
        return false;
    }
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month != 2 || day <= 28 || leap;
}

static bool date_shaped(const char *s, size_t len) {
    return len >= 10 && all_digits(s, 4) && s[4] == '-' && all_digits(s + 5, 2) && s[7] == '-' &&
           all_digits(s + 8, 2);
}

/* HH:MM:SS with an optional fraction: its length, or 0. */
static size_t time_at(const char *s, size_t len) {
    if (len < 8 || !all_digits(s, 2) || s[2] != ':' || !all_digits(s + 3, 2) || s[5] != ':' ||
        !all_digits(s + 6, 2)) {
        return 0;
    }
    if (two(s) > 23 || two(s + 3) > 59 || two(s + 6) > 60) {
        return 0;
    }
    size_t n = 8;
    if (n < len && s[n] == '.') {
        size_t digits = 0;
        while (n + 1 + digits < len && s[n + 1 + digits] >= '0' && s[n + 1 + digits] <= '9') {
            digits++;
        }
        if (digits == 0) {
            return 0;
        }
        n += 1 + digits;
    }
    return n;
}

/* Is the token a date, a time, or both, with an offset or not? */
static bool datetime_ok(const char *s, size_t len) {
    if (date_shaped(s, len)) {
        if (!date_ok(s)) {
            return false;
        }
        if (len == 10) {
            return true; /* a local date */
        }
        if (s[10] != 'T' && s[10] != 't' && s[10] != ' ') {
            return false;
        }
        size_t t = time_at(s + 11, len - 11);
        if (t == 0) {
            return false;
        }
        size_t at = 11 + t;
        if (at == len) {
            return true; /* a local date-time */
        }
        if ((s[at] == 'Z' || s[at] == 'z') && at + 1 == len) {
            return true;
        }
        return (s[at] == '+' || s[at] == '-') && at + 6 == len && all_digits(s + at + 1, 2) &&
               s[at + 3] == ':' && all_digits(s + at + 4, 2) && two(s + at + 1) <= 23 &&
               two(s + at + 4) <= 59;
    }
    return time_at(s, len) == len; /* a local time */
}

/* Digits of `base` with single '_' between them, s[0..len). */
static bool digits_ok(const char *s, size_t len, int base) {
    if (len == 0 || s[0] == '_' || s[len - 1] == '_') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '_') {
            if (s[i + 1] == '_') {
                return false;
            }
            continue;
        }
        int v = c >= '0' && c <= '9'   ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                       : 99;
        if (v >= base) {
            return false;
        }
    }
    return true;
}

/* An integer or float token; false if it is neither. */
static bool parse_number(Parser *p, PlumTomlNode *node, const char *s, size_t len, PlumSpan span) {
    const char *t = s;
    size_t n = len;
    bool negative = false, signed_ = false;
    if (n > 0 && (t[0] == '+' || t[0] == '-')) {
        negative = t[0] == '-';
        signed_ = true;
        t++;
        n--;
    }
    if (n == 3 && (memcmp(t, "inf", 3) == 0 || memcmp(t, "nan", 3) == 0)) {
        node->kind = PLUM_TOML_NODE_FLOAT;
        /* strtod, not NAN: MSVC won't take NAN in constant arithmetic */
        node->f = strtod(t[0] == 'i' ? "inf" : "nan", NULL);
        node->f = negative ? -node->f : node->f;
        return true;
    }
    if (n > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'o' || t[1] == 'b')) {
        int base = t[1] == 'x' ? 16 : t[1] == 'o' ? 8 : 2;
        if (signed_ || !digits_ok(t + 2, n - 2, base)) {
            return false;
        }
        uint64_t v = 0;
        for (size_t i = 2; i < n; i++) {
            if (t[i] == '_') {
                continue;
            }
            int d = t[i] >= '0' && t[i] <= '9' ? t[i] - '0' : (t[i] | 0x20) - 'a' + 10;
            if (v > (UINT64_C(0x7FFFFFFFFFFFFFFF) - (uint64_t)d) / (uint64_t)base) {
                fail(p, span, "this integer doesn't fit in 64 bits");
                return true;
            }
            v = v * (uint64_t)base + (uint64_t)d;
        }
        node->kind = PLUM_TOML_NODE_INTEGER;
        node->i = (int64_t)v;
        return true;
    }
    /* the whole part: no leading zeros */
    size_t whole = 0;
    while (whole < n && ((t[whole] >= '0' && t[whole] <= '9') || t[whole] == '_')) {
        whole++;
    }
    if (whole == 0 || !digits_ok(t, whole, 10) || (t[0] == '0' && whole > 1)) {
        return false;
    }
    if (whole == n) {
        uint64_t v = 0,
                 limit = negative ? UINT64_C(0x8000000000000000) : UINT64_C(0x7FFFFFFFFFFFFFFF);
        for (size_t i = 0; i < n; i++) {
            if (t[i] == '_') {
                continue;
            }
            uint64_t d = (uint64_t)(t[i] - '0');
            if (v > (limit - d) / 10) {
                fail(p, span, "this integer doesn't fit in 64 bits");
                return true;
            }
            v = v * 10 + d;
        }
        node->kind = PLUM_TOML_NODE_INTEGER;
        node->i = negative ? (int64_t)(0 - v) : (int64_t)v;
        return true;
    }
    size_t at = whole;
    if (t[at] == '.') {
        size_t frac = at + 1;
        while (frac < n && ((t[frac] >= '0' && t[frac] <= '9') || t[frac] == '_')) {
            frac++;
        }
        if (!digits_ok(t + at + 1, frac - at - 1, 10)) {
            return false;
        }
        at = frac;
    }
    if (at < n && (t[at] == 'e' || t[at] == 'E')) {
        at++;
        if (at < n && (t[at] == '+' || t[at] == '-')) {
            at++;
        }
        if (!digits_ok(t + at, n - at, 10)) {
            return false;
        }
        at = n;
    }
    if (at != n) {
        return false;
    }
    char *buf = plum_arena_alloc(&p->doc->arena, len + 1);
    size_t k = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] != '_') {
            buf[k++] = s[i];
        }
    }
    buf[k] = '\0';
    node->kind = PLUM_TOML_NODE_FLOAT;
    node->f = strtod(buf, NULL);
    return true;
}

/* A number, a date or time, or true / false, at its first character. */
static PlumTomlNode *parse_scalar(Parser *p) {
    PlumSpan start = here(p);
    const char *s = p->src->text + p->pos;
    size_t len = 0;
    for (;;) {
        char c = peek_at(p, len);
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            c == '_' || c == '+' || c == '-' || c == '.' || c == ':') {
            len++;
        } else if (c == ' ' && len == 10 && date_shaped(s, len) && peek_at(p, len + 1) >= '0' &&
                   peek_at(p, len + 1) <= '9') {
            len++; /* 1979-05-27 07:32:00: a space between date and time */
        } else {
            break;
        }
    }
    if (len == 0) {
        char c = peek(p);
        if (at_end(p) || c == '\n' || c == '\r' || c == '#') {
            fail(p, start, "a value is missing here");
        } else {
            fail(p, start,
                 "unexpected '%c'; a value is a string in quotes, a number, true or "
                 "false, a date, an [array] or an {inline table}",
                 c);
        }
        return NULL;
    }
    for (size_t i = 0; i < len; i++) {
        advance(p);
    }
    PlumSpan span = span_from(p, start);
    PlumTomlNode *node = new_node(p, PLUM_TOML_NODE_BOOL, span);
    if (len == 4 && memcmp(s, "true", 4) == 0) {
        node->b = true;
        return node;
    }
    if (len == 5 && memcmp(s, "false", 5) == 0) {
        return node;
    }
    if ((len >= 10 && date_shaped(s, len)) || (len >= 8 && s[2] == ':')) {
        if (!datetime_ok(s, len)) {
            fail(p, start,
                 "'%.*s' isn't a date or time TOML takes, like 1979-05-27, 07:32:00 or "
                 "1979-05-27T07:32:00Z",
                 (int)(len > 60 ? 60 : len), s);
            return NULL;
        }
        node->kind = PLUM_TOML_NODE_DATETIME;
        char *text = plum_arena_strndup(&p->doc->arena, s, len);
        for (size_t i = 0; i < len; i++) {
            if (i == 10 && (text[i] == 't' || text[i] == ' ')) {
                text[i] = 'T';
            } else if (text[i] == 'z') {
                text[i] = 'Z';
            }
        }
        node->str = text;
        node->len = len;
        return node;
    }
    if (!parse_number(p, node, s, len, start)) {
        fail(p, start,
             "'%.*s' isn't a value TOML takes: a number (42, 3.14, 0xff), true or false, "
             "or a date; text goes in quotes",
             (int)(len > 60 ? 60 : len), s);
        return NULL;
    }
    return p->failed ? NULL : node;
}

static bool add_pair(Parser *p, PlumTomlNode *table, int depth);

/* [a, b, c], over lines if need be, at its '['. */
static PlumTomlNode *parse_array(Parser *p, int depth) {
    PlumSpan start = here(p);
    advance(p);
    PlumTomlNode *array = new_node(p, PLUM_TOML_NODE_ARRAY, start);
    for (;;) {
        skip_blank(p);
        if (p->failed) {
            return NULL;
        }
        if (peek(p) == ']') {
            advance(p);
            return array;
        }
        if (at_end(p)) {
            fail(p, start, "this array has no closing ']'");
            return NULL;
        }
        PlumTomlNode *item = parse_value(p, depth + 1);
        if (item == NULL) {
            return NULL;
        }
        node_add(p, array, no_key, item);
        skip_blank(p);
        if (p->failed) {
            return NULL;
        }
        if (peek(p) == ',') {
            advance(p);
        } else if (peek(p) != ']') {
            fail(p, here(p),
                 at_end(p) ? "this array has no closing ']'" : "expected ',' or ']' in the array");
            return NULL;
        }
    }
}

/* {key = value, ...}, on one line, at its '{'. */
static PlumTomlNode *parse_inline_table(Parser *p, int depth) {
    PlumSpan start = here(p);
    advance(p);
    PlumTomlNode *table = new_node(p, PLUM_TOML_NODE_TABLE, start);
    skip_spaces(p);
    if (peek(p) == '}') {
        advance(p);
        table->flags = MADE_INLINE;
        return table;
    }
    for (;;) {
        if (!add_pair(p, table, depth + 1)) {
            return NULL;
        }
        skip_spaces(p);
        if (peek(p) == ',') {
            advance(p);
            skip_spaces(p);
            if (peek(p) == '}') {
                fail(p, here(p), "no ',' after an inline table's last value");
                return NULL;
            }
            continue;
        }
        if (peek(p) == '}') {
            advance(p);
            break;
        }
        fail(p, here(p), "expected ',' or '}': an inline table stays on one line");
        return NULL;
    }
    table->flags = MADE_INLINE; /* closed: nothing more may be added to it */
    return table;
}

static PlumTomlNode *parse_value(Parser *p, int depth) {
    if (depth > MAX_DEPTH) {
        fail(p, here(p), "arrays and inline tables nested more than %d deep", MAX_DEPTH);
        return NULL;
    }
    char c = peek(p);
    if (c == '"' || c == '\'') {
        PlumSpan start = here(p);
        size_t len = 0;
        const char *s = parse_string(p, &len);
        if (s == NULL) {
            return NULL;
        }
        PlumTomlNode *node = new_node(p, PLUM_TOML_NODE_STRING, span_from(p, start));
        node->str = s;
        node->len = len;
        return node;
    }
    if (c == '[') {
        return parse_array(p, depth);
    }
    if (c == '{') {
        return parse_inline_table(p, depth);
    }
    return parse_scalar(p);
}

/*
 * key = value into `table` (the current table, or an inline one): a
 * dotted key makes or goes into tables that dotted keys made.
 */
static bool add_pair(Parser *p, PlumTomlNode *table, int depth) {
    PlumSpan key_start = here(p);
    Parts parts = {0};
    if (!parse_dotted_key(p, &parts, "key")) {
        plum_array_free(&parts);
        return false;
    }
    PlumSpan key_span = span_from(p, key_start);
    if (peek(p) != '=') {
        fail(p, here(p), "expected '=' after the key '%s'", plum_array_last(&parts).s);
        plum_array_free(&parts);
        return false;
    }
    advance(p);
    skip_spaces(p);
    PlumTomlNode *node = table;
    for (size_t i = 0; i + 1 < parts.len; i++) {
        PlumTomlNode *child = node_find(node, parts.items[i]);
        if (child == NULL) {
            child = new_node(p, PLUM_TOML_NODE_TABLE, key_span);
            child->flags = MADE_DOTTED;
            node_add(p, node, parts.items[i], child);
        } else if (child->kind != PLUM_TOML_NODE_TABLE || (child->flags & MADE_DOTTED) == 0) {
            fail_twice(p, key_span,
                       "'%s' is already %s, defined on line %u; a dotted key can't "
                       "add to it",
                       parts.items[i].s, describe(child), (unsigned)child->span.line);
            plum_array_free(&parts);
            return false;
        }
        node = child;
    }
    Key last = plum_array_last(&parts);
    plum_array_free(&parts);
    PlumTomlNode *old = node_find(node, last);
    if (old != NULL) {
        fail_twice(p, key_span, "'%s' is already set on line %u; keep one", last.s,
                   (unsigned)old->span.line);
        return false;
    }
    PlumTomlNode *value = parse_value(p, depth);
    if (value == NULL) {
        return false;
    }
    node_add(p, node, last, value);
    return true;
}

/* [a.b.c] or [[a.b.c]], at its '['. */
static void parse_header_full(Parser *p) {
    PlumSpan start = here(p);
    advance(p);
    bool aot = peek(p) == '[';
    if (aot) {
        advance(p);
    }
    Parts parts = {0};
    if (!parse_dotted_key(p, &parts, "table name")) {
        plum_array_free(&parts);
        return;
    }
    if (peek(p) != ']' || (aot && peek_at(p, 1) != ']')) {
        fail(p, here(p),
             aot ? "expected ']]' to close the table name"
                 : "expected ']' to close the table name");
        plum_array_free(&parts);
        return;
    }
    advance(p);
    if (aot) {
        advance(p);
    }
    PlumSpan span = span_from(p, start);
    if (!end_of_line(p)) {
        plum_array_free(&parts);
        return;
    }
    PlumTomlNode *node = p->doc->root;
    for (size_t i = 0; i + 1 < parts.len; i++) {
        PlumTomlNode *child = node_find(node, parts.items[i]);
        if (child == NULL) {
            child = new_node(p, PLUM_TOML_NODE_TABLE, span);
            child->flags = MADE_IMPLICIT;
            node_add(p, node, parts.items[i], child);
        } else if (child->kind == PLUM_TOML_NODE_ARRAY && (child->flags & MADE_AOT) != 0) {
            child = child->items[child->count - 1];
        } else if (child->kind != PLUM_TOML_NODE_TABLE || (child->flags & MADE_INLINE) != 0) {
            fail_twice(p, span, "'%s' is already %s, defined on line %u; a [table] can't go in it",
                       parts.items[i].s, describe(child), (unsigned)child->span.line);
            plum_array_free(&parts);
            return;
        }
        node = child;
    }
    Key last = plum_array_last(&parts);
    plum_array_free(&parts);
    PlumTomlNode *old = node_find(node, last);
    if (aot) {
        PlumTomlNode *array = old;
        if (array == NULL) {
            array = new_node(p, PLUM_TOML_NODE_ARRAY, span);
            array->flags = MADE_AOT;
            node_add(p, node, last, array);
        } else if (array->kind != PLUM_TOML_NODE_ARRAY || (array->flags & MADE_AOT) == 0) {
            fail_twice(p, span,
                       "'%s' is already %s, defined on line %u; it can't be an array of "
                       "tables too",
                       last.s, describe(old), (unsigned)old->span.line);
            return;
        }
        PlumTomlNode *table = new_node(p, PLUM_TOML_NODE_TABLE, span);
        table->flags = MADE_HEADER;
        node_add(p, array, no_key, table);
        p->current = table;
        return;
    }
    if (old == NULL) {
        old = new_node(p, PLUM_TOML_NODE_TABLE, span);
        old->flags = MADE_HEADER;
        node_add(p, node, last, old);
    } else if (old->kind == PLUM_TOML_NODE_TABLE && old->flags == MADE_IMPLICIT) {
        old->flags = MADE_HEADER; /* [a.b] came first; now [a] itself */
        old->span = span;
    } else {
        fail_twice(p, span, "'%s' is already %s, defined on line %u; define a table once", last.s,
                   describe(old), (unsigned)old->span.line);
        return;
    }
    p->current = old;
}

/* ------------------------------------------------------------------ */
/* Parsing a document                                                  */
/* ------------------------------------------------------------------ */

static bool parse_document(PlumToml *doc, const PlumSource *source, PlumDiagList *diags,
                           bool full) {
    memset(doc, 0, sizeof *doc);
    plum_arena_init(&doc->arena);
    Parser p;
    memset(&p, 0, sizeof p);
    p.doc = doc;
    p.src = source;
    p.diags = diags;
    p.line = 1;
    p.column = 1;
    p.table = "";
    p.full = full;
    if (full) {
        PlumSpan top = {0, 0, 1, 1};
        doc->root = new_node(&p, PLUM_TOML_NODE_TABLE, top);
        doc->root->flags = MADE_HEADER;
        p.current = doc->root;
    }
    if (source->length >= 3 && memcmp(source->text, "\xEF\xBB\xBF", 3) == 0) {
        p.pos = 3; /* a byte order mark */
    }
    if (!plum_utf8_valid(source->text + p.pos, source->length - p.pos)) {
        PlumSpan none = {0, 0, 1, 1};
        plum_diag_error(diags, none, "E0901", "this file isn't UTF-8 text");
        return false;
    }
    while (!at_end(&p) && !p.failed) {
        skip_spaces(&p);
        char c = peek(&p);
        if (c == '\n' || c == '\r' || c == '#' || at_end(&p)) {
            end_of_line(&p);
        } else if (c == '[') {
            if (full) {
                parse_header_full(&p);
            } else {
                parse_header(&p);
            }
        } else if (full) {
            if (add_pair(&p, p.current, 0)) {
                end_of_line(&p);
            }
        } else {
            parse_pair(&p);
        }
    }
    return !p.failed;
}

bool plum_toml_parse(PlumToml *doc, const PlumSource *source, PlumDiagList *diags) {
    return parse_document(doc, source, diags, false);
}

bool plum_toml_parse_full(PlumToml *doc, const PlumSource *source, PlumDiagList *diags) {
    return parse_document(doc, source, diags, true);
}

void plum_toml_free(PlumToml *doc) {
    plum_array_free(&doc->values);
    plum_array_free(&doc->tables);
    plum_arena_free(&doc->arena);
    doc->root = NULL;
}

const PlumTomlValue *plum_toml_get(const PlumToml *doc, const char *table, const char *key) {
    for (size_t i = 0; i < doc->values.len; i++) {
        const PlumTomlValue *v = &doc->values.items[i];
        if (strcmp(v->table, table) == 0 && strcmp(v->key, key) == 0) {
            return v;
        }
    }
    return NULL;
}

const PlumTomlTable *plum_toml_table(const PlumToml *doc, const char *name) {
    for (size_t i = 0; i < doc->tables.len; i++) {
        if (strcmp(doc->tables.items[i].name, name) == 0) {
            return &doc->tables.items[i];
        }
    }
    return NULL;
}
