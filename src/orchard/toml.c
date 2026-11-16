/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "orchard/toml.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

bool plum_utf8_valid(const char *s, size_t len) {
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < len) {
        unsigned c = p[i];
        size_t extra;
        uint32_t cp;
        if (c == 0) {
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

typedef struct Parser {
    PlumToml *doc;
    const PlumSource *src;
    PlumDiagList *diags;
    size_t pos;
    uint32_t line;
    uint32_t column;
    const char *table;        /* the current table */
    PlumTomlTable *table_rec; /* NULL at the top level */
    size_t table_index;
    bool failed;
} Parser;

static char peek(const Parser *p) {
    return p->pos < p->src->length ? p->src->text[p->pos] : '\0';
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

static void skip_spaces(Parser *p) {
    while (peek(p) == ' ' || peek(p) == '\t') {
        advance(p);
    }
}

/* Spaces, comments and line breaks (inside an array). */
static void skip_blank(Parser *p) {
    for (;;) {
        char c = peek(p);
        if (c == ' ' || c == '\t' || c == '\n' ||
            (c == '\r' && p->pos + 1 < p->src->length && p->src->text[p->pos + 1] == '\n')) {
            advance(p);
        } else if (c == '#') {
            while (!at_end(p) && peek(p) != '\n') {
                advance(p);
            }
        } else {
            return;
        }
    }
}

/* After a value or header: spaces, maybe a comment, then the line's end. */
static bool end_of_line(Parser *p) {
    skip_spaces(p);
    if (peek(p) == '#') {
        while (!at_end(p) && peek(p) != '\n') {
            unsigned char c = (unsigned char)peek(p);
            if (c < 0x20 && c != '\t' && c != '\r') {
                fail(p, here(p), "a control character in a comment");
                return false;
            }
            advance(p);
        }
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

/* A "basic" or 'literal' string, at its opening quote. NULL after an error. */
static const char *parse_string(Parser *p) {
    PlumSpan start = here(p);
    char quote = peek(p);
    advance(p);
    if (peek(p) == quote && p->pos + 1 < p->src->length && p->src->text[p->pos + 1] == quote) {
        fail(p, start, "multi-line strings aren't supported here; write the text on one line");
        return NULL;
    }
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
            PlumSpan esc = here(p);
            advance(p);
            char e = peek(p);
            advance(p);
            switch (e) {
            case '"':
            case '\\':
                plum_array_push(&out, e);
                break;
            case 'n':
                plum_array_push(&out, '\n');
                break;
            case 't':
                plum_array_push(&out, '\t');
                break;
            case 'r':
                plum_array_push(&out, '\r');
                break;
            case 'b':
                plum_array_push(&out, '\b');
                break;
            case 'f':
                plum_array_push(&out, '\f');
                break;
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
                        plum_array_free(&out);
                        return NULL;
                    }
                    cp = cp * 16 + (uint32_t)v;
                    advance(p);
                }
                if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                    fail(p, esc, "\\%c names no character a string can hold", e);
                    plum_array_free(&out);
                    return NULL;
                }
                put_utf8(&out, cp);
                break;
            }
            default:
                fail(p, esc, "an unknown escape; use \\\\ for a backslash");
                plum_array_free(&out);
                return NULL;
            }
            continue;
        }
        plum_array_push(&out, (char)c);
        advance(p);
    }
    if (!plum_utf8_valid(out.items != NULL ? out.items : "", out.len)) {
        fail(p, start, "this string isn't valid UTF-8");
        plum_array_free(&out);
        return NULL;
    }
    char *s = plum_arena_strndup(&p->doc->arena, out.items != NULL ? out.items : "", out.len);
    plum_array_free(&out);
    return s;
}

/* A bare or quoted key. NULL after an error. */
static const char *parse_key(Parser *p, const char *what) {
    if (peek(p) == '"' || peek(p) == '\'') {
        const char *k = parse_string(p);
        if (k != NULL && k[0] == '\0') {
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
        const char *part = parse_key(p, "table name");
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
            plum_diag_error(p->diags, span, "E0902",
                            "the table [%s] is already declared on line %u; keep one", name,
                            (unsigned)p->doc->tables.items[i].span.line);
            p->failed = true;
            return;
        }
    }
    for (size_t i = 0; i < p->doc->values.len; i++) {
        const PlumTomlValue *v = &p->doc->values.items[i];
        if (strcmp(join(p, v->table, v->key), name) == 0) {
            plum_diag_error(p->diags, span, "E0902",
                            "[%s] is already a key, on line %u; a name can't be both", name,
                            (unsigned)v->key_span.line);
            p->failed = true;
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
    const char *key = parse_key(p, "key");
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
    if (c == '"' || c == '\'') {
        v.kind = PLUM_TOML_STRING;
        v.str = parse_string(p);
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
            const char *s = parse_string(p);
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
            plum_diag_error(p->diags, key_span, "E0902", "'%s' is already set on line %u; keep one",
                            key, (unsigned)o->key_span.line);
            p->failed = true;
            return;
        }
    }
    const char *full = join(p, v.table, v.key);
    if (plum_toml_table(p->doc, full) != NULL) {
        plum_diag_error(p->diags, key_span, "E0902",
                        "'%s' is already a [table]; a name can't be both", full);
        p->failed = true;
        return;
    }
    plum_array_push(&p->doc->values, v);
    if (p->table_rec != NULL) {
        p->doc->tables.items[p->table_index].last_line = v.last_line;
    }
}

bool plum_toml_parse(PlumToml *doc, const PlumSource *source, PlumDiagList *diags) {
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
            parse_header(&p);
        } else {
            parse_pair(&p);
        }
    }
    return !p.failed;
}

void plum_toml_free(PlumToml *doc) {
    plum_array_free(&doc->values);
    plum_array_free(&doc->tables);
    plum_arena_free(&doc->arena);
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
