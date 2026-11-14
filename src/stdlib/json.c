/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The json module, and the methods of Json values.
 *
 * A Json value is an ordinary value: none (null), bool, int or float (a
 * number), str, a list, or a map with str keys (an object). Parsing keeps
 * an object's keys in their order; a number without a fraction or exponent
 * that fits is an int. Writing also takes tree objects (as their fields)
 * and sets (as lists).
 */
#include "stdlib/native.h"

#include "util/array.h"
#include "util/fmt.h"
#include "vm/table.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ARGS PlumNative *n, const PlumValue *args, int argc, PlumValue *result

/* How deep arrays and objects may nest, in either direction. */
#define MAX_DEPTH 512

typedef PLUM_ARRAY(char) Buf;

static void put(Buf *b, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        plum_array_push(b, s[i]);
    }
}

static void puts_(Buf *b, const char *s) {
    put(b, s, strlen(s));
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

typedef struct Parser {
    PlumNative *n;
    const char *s;
    size_t len;
    size_t at;
    int depth;
} Parser;

/* A ParseError naming the line and column of the current position. */
static bool fail(Parser *p, const char *what) {
    size_t line = 1, col = 1;
    for (size_t i = 0; i < p->at && i < p->len; i++) {
        if (p->s[i] == '\n') {
            line++;
            col = 1;
        } else if (((unsigned char)p->s[i] & 0xC0) != 0x80) {
            col++;
        }
    }
    return plum_native_throw(p->n, PLUM_ERROR_PARSE, "json.parse: line %zu, column %zu: %s", line,
                             col, what);
}

static void skip_space(Parser *p) {
    while (p->at < p->len && (p->s[p->at] == ' ' || p->s[p->at] == '\t' || p->s[p->at] == '\n' ||
                              p->s[p->at] == '\r')) {
        p->at++;
    }
}

static bool literal(Parser *p, const char *word) {
    size_t n = strlen(word);
    if (p->len - p->at >= n && memcmp(p->s + p->at, word, n) == 0) {
        p->at += n;
        return true;
    }
    return false;
}

static void put_utf8(Buf *b, uint32_t cp) {
    char out[4];
    size_t k;
    if (cp < 0x80) {
        out[0] = (char)cp;
        k = 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        k = 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        k = 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        k = 4;
    }
    put(b, out, k);
}

static bool hex4(Parser *p, uint32_t *out) {
    if (p->len - p->at < 4) {
        return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p->s[p->at++];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v |= (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v |= (uint32_t)(c - 'A' + 10);
        } else {
            return false;
        }
    }
    *out = v;
    return true;
}

/* A string, after its opening quote. */
static bool parse_string(Parser *p, PlumValue *out) {
    Buf b = {0};
    for (;;) {
        if (p->at >= p->len) {
            plum_array_free(&b);
            return fail(p, "this string has no closing '\"'");
        }
        unsigned char c = (unsigned char)p->s[p->at++];
        if (c == '"') {
            break;
        }
        if (c < 0x20) {
            p->at--;
            plum_array_free(&b);
            return fail(p, "a string can't contain a line break or control character; write \\n");
        }
        if (c != '\\') {
            plum_array_push(&b, (char)c);
            continue;
        }
        if (p->at >= p->len) {
            plum_array_free(&b);
            return fail(p, "this string has no closing '\"'");
        }
        char e = p->s[p->at++];
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
            uint32_t cp;
            if (!hex4(p, &cp)) {
                plum_array_free(&b);
                return fail(p, "\\u needs four hex digits");
            }
            if (cp >= 0xD800 && cp < 0xDC00) {
                /* a surrogate pair: 😀 */
                uint32_t low;
                if (!(literal(p, "\\u") && hex4(p, &low) && low >= 0xDC00 && low < 0xE000)) {
                    plum_array_free(&b);
                    return fail(p, "a \\u surrogate must come in a pair");
                }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            } else if (cp >= 0xDC00 && cp < 0xE000) {
                plum_array_free(&b);
                return fail(p, "a \\u surrogate must come in a pair");
            }
            put_utf8(&b, cp);
            break;
        }
        default:
            p->at--;
            plum_array_free(&b);
            return fail(p, "unknown escape in a string");
        }
    }
    *out = plum_str_value(b.items != NULL ? b.items : "", b.len);
    plum_array_free(&b);
    return true;
}

static bool parse_number(Parser *p, PlumValue *out) {
    size_t start = p->at;
    bool integral = true;
    if (p->at < p->len && p->s[p->at] == '-') {
        p->at++;
    }
    size_t digits = p->at;
    while (p->at < p->len && p->s[p->at] >= '0' && p->s[p->at] <= '9') {
        p->at++;
    }
    if (p->at == digits || (p->s[digits] == '0' && p->at - digits > 1)) {
        p->at = start;
        return fail(p, "this isn't a number JSON allows");
    }
    if (p->at < p->len && p->s[p->at] == '.') {
        integral = false;
        p->at++;
        size_t frac = p->at;
        while (p->at < p->len && p->s[p->at] >= '0' && p->s[p->at] <= '9') {
            p->at++;
        }
        if (p->at == frac) {
            return fail(p, "a '.' in a number needs digits after it");
        }
    }
    if (p->at < p->len && (p->s[p->at] == 'e' || p->s[p->at] == 'E')) {
        integral = false;
        p->at++;
        if (p->at < p->len && (p->s[p->at] == '+' || p->s[p->at] == '-')) {
            p->at++;
        }
        size_t exp = p->at;
        while (p->at < p->len && p->s[p->at] >= '0' && p->s[p->at] <= '9') {
            p->at++;
        }
        if (p->at == exp) {
            return fail(p, "an exponent needs digits");
        }
    }
    char text[64];
    size_t len = p->at - start;
    if (len >= sizeof text) {
        integral = false; /* a very long number: as a float */
        len = sizeof text - 1;
    }
    memcpy(text, p->s + start, len);
    text[len] = '\0';
    if (integral) {
        char *end;
        long long v = strtoll(text, &end, 10);
        if (*end == '\0' && v != LLONG_MAX && v != LLONG_MIN) {
            *out = plum_int((int64_t)v);
            return true;
        }
    }
    *out = plum_float(strtod(text, NULL)); /* too large for an int: a float */
    return true;
}

static bool parse_value(Parser *p, PlumValue *out);

static bool parse_array(Parser *p, PlumValue *out) {
    PlumList *list = plum_list_new(0);
    skip_space(p);
    if (p->at < p->len && p->s[p->at] == ']') {
        p->at++;
        *out = plum_obj(&list->obj);
        return true;
    }
    for (;;) {
        PlumValue item;
        if (!parse_value(p, &item)) {
            plum_release(plum_obj(&list->obj));
            return false;
        }
        plum_list_push(list, item);
        skip_space(p);
        if (p->at < p->len && p->s[p->at] == ',') {
            p->at++;
            continue;
        }
        if (p->at < p->len && p->s[p->at] == ']') {
            p->at++;
            break;
        }
        plum_release(plum_obj(&list->obj));
        return fail(p, "expected ',' or ']' in the list");
    }
    *out = plum_obj(&list->obj);
    return true;
}

static bool parse_object(Parser *p, PlumValue *out) {
    PlumTable *map = plum_native_new_map(p->n);
    PlumValue result = plum_obj(&map->obj);
    skip_space(p);
    if (p->at < p->len && p->s[p->at] == '}') {
        p->at++;
        *out = result;
        return true;
    }
    for (;;) {
        skip_space(p);
        PlumValue key, value;
        if (p->at >= p->len || p->s[p->at] != '"') {
            plum_release(result);
            return fail(p, "expected a key in quotes");
        }
        p->at++;
        if (!parse_string(p, &key)) {
            plum_release(result);
            return false;
        }
        skip_space(p);
        if (p->at >= p->len || p->s[p->at] != ':') {
            plum_release(key);
            plum_release(result);
            return fail(p, "expected ':' after the key");
        }
        p->at++;
        if (!parse_value(p, &value)) {
            plum_release(key);
            plum_release(result);
            return false;
        }
        plum_table_put(map, key, value); /* a repeated key keeps the last value */
        skip_space(p);
        if (p->at < p->len && p->s[p->at] == ',') {
            p->at++;
            continue;
        }
        if (p->at < p->len && p->s[p->at] == '}') {
            p->at++;
            break;
        }
        plum_release(result);
        return fail(p, "expected ',' or '}' in the object");
    }
    *out = result;
    return true;
}

static bool parse_value(Parser *p, PlumValue *out) {
    skip_space(p);
    if (p->at >= p->len) {
        return fail(p, "expected a value, found the end");
    }
    char c = p->s[p->at];
    if (c == '{' || c == '[') {
        if (++p->depth > MAX_DEPTH) {
            return fail(p, "nested more than 512 deep");
        }
        p->at++;
        bool ok = c == '{' ? parse_object(p, out) : parse_array(p, out);
        p->depth--;
        return ok;
    }
    if (c == '"') {
        p->at++;
        return parse_string(p, out);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        return parse_number(p, out);
    }
    if (literal(p, "true")) {
        *out = plum_bool(true);
        return true;
    }
    if (literal(p, "false")) {
        *out = plum_bool(false);
        return true;
    }
    if (literal(p, "null")) {
        *out = plum_none();
        return true;
    }
    return fail(p, "expected a value: an object, list, string, number, true, false or null");
}

static bool j_parse(ARGS) {
    (void)argc;
    const PlumString *text = plum_as_string(args[0]);
    Parser p = {n, text->chars, text->len, 0, 0};
    PlumValue v;
    if (!parse_value(&p, &v)) {
        return false;
    }
    skip_space(&p);
    if (p.at < p.len) {
        plum_release(v);
        return fail(&p, "more text after the value");
    }
    *result = v;
    return true;
}

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

typedef struct Writer {
    PlumNative *n;
    Buf out;
    int64_t indent;
} Writer;

static void put_quoted(Buf *b, const char *s, size_t len) {
    plum_array_push(b, '"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':
            puts_(b, "\\\"");
            break;
        case '\\':
            puts_(b, "\\\\");
            break;
        case '\n':
            puts_(b, "\\n");
            break;
        case '\r':
            puts_(b, "\\r");
            break;
        case '\t':
            puts_(b, "\\t");
            break;
        default:
            if (c < 0x20) {
                char esc[8];
                snprintf(esc, sizeof esc, "\\u%04x", c);
                puts_(b, esc);
            } else {
                plum_array_push(b, (char)c);
            }
        }
    }
    plum_array_push(b, '"');
}

static void newline(Writer *w, int depth) {
    if (w->indent <= 0) {
        return;
    }
    plum_array_push(&w->out, '\n');
    for (int64_t i = 0; i < w->indent * depth; i++) {
        plum_array_push(&w->out, ' ');
    }
}

static bool write_value(Writer *w, PlumValue v, int depth);

/* One "key": value of an object. */
static bool write_member(Writer *w, const char *key, size_t len, PlumValue v, int depth,
                         bool first) {
    if (!first) {
        plum_array_push(&w->out, ',');
    }
    newline(w, depth + 1);
    put_quoted(&w->out, key, len);
    puts_(&w->out, w->indent > 0 ? ": " : ":");
    return write_value(w, v, depth + 1);
}

static bool write_value(Writer *w, PlumValue v, int depth) {
    if (depth > MAX_DEPTH) {
        return plum_native_throw(w->n, PLUM_ERROR_VALUE,
                                 "json.stringify: nested more than 512 deep (does an object refer "
                                 "to itself?)");
    }
    char tmp[PLUM_FLOAT_BUF + 8];
    switch (v.type) {
    case PLUM_VAL_NONE:
        puts_(&w->out, "null");
        return true;
    case PLUM_VAL_BOOL:
        puts_(&w->out, v.as.b ? "true" : "false");
        return true;
    case PLUM_VAL_INT:
        snprintf(tmp, sizeof tmp, "%lld", (long long)v.as.i);
        puts_(&w->out, tmp);
        return true;
    case PLUM_VAL_FLOAT:
        if (!isfinite(v.as.f)) {
            return plum_native_throw(w->n, PLUM_ERROR_VALUE,
                                     "json.stringify: JSON has no infinity or NAN");
        }
        puts_(&w->out, plum_format_float(v.as.f, tmp));
        return true;
    case PLUM_VAL_OBJ:
        break;
    }
    switch ((PlumObjType)v.as.obj->type) {
    case PLUM_OBJ_STRING: {
        const PlumString *s = plum_as_string(v);
        put_quoted(&w->out, s->chars, s->len);
        return true;
    }
    case PLUM_OBJ_LIST: {
        const PlumList *list = plum_as_list(v);
        plum_array_push(&w->out, '[');
        for (size_t i = 0; i < list->len; i++) {
            if (i > 0) {
                plum_array_push(&w->out, ',');
            }
            newline(w, depth + 1);
            if (!write_value(w, list->items[i], depth + 1)) {
                return false;
            }
        }
        if (list->len > 0) {
            newline(w, depth);
        }
        plum_array_push(&w->out, ']');
        return true;
    }
    case PLUM_OBJ_MAP: {
        const PlumTable *t = plum_as_table(v);
        plum_array_push(&w->out, t->is_set ? '[' : '{');
        int64_t pos = 0;
        bool first = true;
        const PlumTableEntry *e;
        while ((e = plum_table_next(t, &pos)) != NULL) {
            if (t->is_set) {
                if (!first) {
                    plum_array_push(&w->out, ',');
                }
                newline(w, depth + 1);
                if (!write_value(w, e->key, depth + 1)) {
                    return false;
                }
            } else {
                if (!plum_is_obj(e->key, PLUM_OBJ_STRING)) {
                    return plum_native_throw(w->n, PLUM_ERROR_VALUE,
                                             "json.stringify: an object's keys must be str");
                }
                const PlumString *k = plum_as_string(e->key);
                if (!write_member(w, k->chars, k->len, e->value, depth, first)) {
                    return false;
                }
            }
            first = false;
        }
        if (!first) {
            newline(w, depth);
        }
        plum_array_push(&w->out, t->is_set ? ']' : '}');
        return true;
    }
    case PLUM_OBJ_OBJECT: {
        const PlumObject *o = plum_as_object(v);
        if (o->tree->kind != NULL) {
            return plum_native_throw(w->n, PLUM_ERROR_VALUE,
                                     "json.stringify: a kind's value (%s) has no JSON form",
                                     o->tree->name);
        }
        /* a tree object: its fields, in order */
        plum_array_push(&w->out, '{');
        for (int i = 0; i < o->tree->field_count; i++) {
            PlumValue field = o->fields[i];
            if (plum_is_obj(field, PLUM_OBJ_WEAK)) {
                PlumObject *target = ((PlumWeak *)field.as.obj)->target;
                field = target != NULL ? plum_obj(&target->obj) : plum_none();
            }
            const char *name = o->tree->field_names[i];
            if (!write_member(w, name, strlen(name), field, depth, i == 0)) {
                return false;
            }
        }
        if (o->tree->field_count > 0) {
            newline(w, depth);
        }
        plum_array_push(&w->out, '}');
        return true;
    }
    default:
        return plum_native_throw(w->n, PLUM_ERROR_VALUE,
                                 "json.stringify: a function has no JSON form");
    }
}

static bool j_stringify(ARGS) {
    int64_t indent = argc > 1 && args[1].type == PLUM_VAL_INT ? args[1].as.i : 0;
    if (indent < 0 || indent > 16) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "json.stringify: indent must be 0 to 16, not %lld",
                                 (long long)indent);
    }
    Writer w = {n, {0}, indent};
    if (!write_value(&w, args[0], 0)) {
        plum_array_free(&w.out);
        return false;
    }
    *result = plum_str_value(w.out.items != NULL ? w.out.items : "", w.out.len);
    plum_array_free(&w.out);
    return true;
}

/* ------------------------------------------------------------------ */
/* The methods of Json values                                          */
/* ------------------------------------------------------------------ */

static bool is(PlumValue v, PlumObjType type) {
    return plum_is_obj(v, type);
}

static bool is_object(PlumValue v) {
    return is(v, PLUM_OBJ_MAP) && !plum_as_table(v)->is_set;
}

/* get(key): the member, or null when it's missing or this isn't an object. */
static bool v_get(ARGS) {
    (void)n;
    (void)argc;
    *result = plum_none();
    if (is_object(args[0])) {
        const PlumValue *found = plum_table_find(plum_as_table(args[0]), args[1]);
        if (found != NULL) {
            *result = plum_retain(*found);
        }
    }
    return true;
}

static bool v_at(ARGS) {
    (void)n;
    (void)argc;
    *result = plum_none();
    if (is(args[0], PLUM_OBJ_LIST)) {
        const PlumList *list = plum_as_list(args[0]);
        int64_t i = args[1].as.i;
        if (i < 0) {
            i += (int64_t)list->len; /* -1 is the last */
        }
        if (i >= 0 && (uint64_t)i < list->len) {
            *result = plum_retain(list->items[i]);
        }
    }
    return true;
}

static bool v_len(ARGS) {
    (void)n;
    (void)argc;
    PlumValue v = args[0];
    int64_t len = is(v, PLUM_OBJ_LIST)     ? (int64_t)plum_as_list(v)->len
                  : is(v, PLUM_OBJ_MAP)    ? (int64_t)plum_as_table(v)->count
                  : is(v, PLUM_OBJ_STRING) ? (int64_t)plum_string_length(plum_as_string(v))
                                           : 0;
    *result = plum_int(len);
    return true;
}

static bool v_keys(ARGS) {
    (void)n;
    (void)argc;
    PlumList *out = plum_list_new(0);
    if (is_object(args[0])) {
        int64_t pos = 0;
        const PlumTableEntry *e;
        while ((e = plum_table_next(plum_as_table(args[0]), &pos)) != NULL) {
            plum_list_push(out, plum_retain(e->key));
        }
    }
    *result = plum_obj(&out->obj);
    return true;
}

static const char *kind_of(PlumValue v) {
    switch (v.type) {
    case PLUM_VAL_NONE:
        return "null";
    case PLUM_VAL_BOOL:
        return "bool";
    case PLUM_VAL_INT:
    case PLUM_VAL_FLOAT:
        return "number";
    case PLUM_VAL_OBJ:
        break;
    }
    return is(v, PLUM_OBJ_STRING) ? "string"
           : is(v, PLUM_OBJ_LIST) ? "list"
           : is_object(v)         ? "object"
                                  : "other";
}

static bool v_kind(ARGS) {
    (void)n;
    (void)argc;
    const char *k = kind_of(args[0]);
    *result = plum_str_value(k, strlen(k));
    return true;
}

static bool v_is_null(ARGS) {
    (void)n;
    (void)argc;
    *result = plum_bool(args[0].type == PLUM_VAL_NONE);
    return true;
}

static bool v_str(ARGS) {
    (void)n;
    (void)argc;
    *result = is(args[0], PLUM_OBJ_STRING) ? plum_retain(args[0]) : plum_none();
    return true;
}

static bool v_int(ARGS) {
    (void)n;
    (void)argc;
    PlumValue v = args[0];
    *result = plum_none();
    if (v.type == PLUM_VAL_INT) {
        *result = v;
    } else if (v.type == PLUM_VAL_FLOAT && v.as.f == floor(v.as.f) && fabs(v.as.f) < 9.2e18) {
        *result = plum_int((int64_t)v.as.f); /* 3.0 is the int 3; 3.5 isn't an int */
    }
    return true;
}

static bool v_float(ARGS) {
    (void)n;
    (void)argc;
    PlumValue v = args[0];
    *result = v.type == PLUM_VAL_INT     ? plum_float((double)v.as.i)
              : v.type == PLUM_VAL_FLOAT ? v
                                         : plum_none();
    return true;
}

static bool v_bool(ARGS) {
    (void)n;
    (void)argc;
    *result = args[0].type == PLUM_VAL_BOOL ? args[0] : plum_none();
    return true;
}

static bool v_list(ARGS) {
    (void)n;
    (void)argc;
    *result = is(args[0], PLUM_OBJ_LIST) ? plum_retain(args[0]) : plum_none();
    return true;
}

static bool v_map(ARGS) {
    (void)n;
    (void)argc;
    *result = is_object(args[0]) ? plum_retain(args[0]) : plum_none();
    return true;
}

static const PlumNativeFunction functions[] = {
    {"parse", "(text: str) -> Json", j_parse,
     "JSON text as a Json value; a ParseError says where it went wrong"},
    {"stringify", "(value: Json, indent: int = 0) -> str", j_stringify,
     "a value as JSON text, on one line or indented by that many spaces"},
};

const PlumNativeModule plum_module_json = {
    "json", "parse and produce JSON", functions, sizeof functions / sizeof functions[0], NULL, 0,
};

static const PlumNativeFunction methods[] = {
    {"get", "(self: Json, key: str) -> Json", v_get,
     "an object's member; null if it's missing or this isn't an object"},
    {"at", "(self: Json, index: int) -> Json", v_at,
     "a list's item (-1 is the last); null if out of range or this isn't a list"},
    {"len", "(self: Json) -> int", v_len,
     "the items of a list, members of an object or characters of a string; else 0"},
    {"keys", "(self: Json) -> list[str]", v_keys, "an object's keys in order; else []"},
    {"type_name", "(self: Json) -> str", v_kind,
     "\"null\", \"bool\", \"number\", \"string\", \"list\" or \"object\""},
    {"is_null", "(self: Json) -> bool", v_is_null, "is this null?"},
    {"str", "(self: Json) -> str?", v_str, "the string, or none if this isn't one"},
    {"int", "(self: Json) -> int?", v_int, "the number as an int, or none if it isn't a whole one"},
    {"float", "(self: Json) -> float?", v_float, "the number as a float, or none"},
    {"bool", "(self: Json) -> bool?", v_bool, "the bool, or none"},
    {"list", "(self: Json) -> list[Json]?", v_list, "the list, or none"},
    {"map", "(self: Json) -> map[str, Json]?", v_map, "the object as a map, or none"},
};

const PlumNativeModule plum_module_json_value = {
    "json.value", "the methods of Json values (no seed needed)",
    methods,      sizeof methods / sizeof methods[0],
    NULL,         0,
};
