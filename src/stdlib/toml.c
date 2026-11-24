/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The toml module: TOML 1.0 text to Json values and back. Parsing is the
 * parser plum.toml is read with (src/orchard/toml.c), in its full mode.
 *
 * Tables are maps (in the order their keys came), arrays lists, strings
 * str, integers int, floats float, booleans bool. Dates and times are
 * their text, as RFC 3339 writes it: TOML has them, Plumlang doesn't yet.
 */
#include "stdlib/native.h"

#include "orchard/toml.h"
#include "util/array.h"
#include "util/fmt.h"
#include "vm/table.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define ARGS PlumNative *n, const PlumValue *args, int argc, PlumValue *result

typedef PLUM_ARRAY(char) Buf;

static void put(Buf *b, const char *s, size_t len) {
    if (len == 0) {
        return;
    }
    plum_array_reserve(b, len);
    memcpy(b->items + b->len, s, len);
    b->len += len;
}

static void put_str(Buf *b, const char *s) {
    put(b, s, strlen(s));
}

/* ------------------------------------------------------------------ */
/* parse                                                               */
/* ------------------------------------------------------------------ */

static PlumValue to_value(PlumNative *n, const PlumTomlNode *node) {
    switch (node->kind) {
    case PLUM_TOML_NODE_STRING:
    case PLUM_TOML_NODE_DATETIME:
        return plum_str_value(node->str, node->len);
    case PLUM_TOML_NODE_INTEGER:
        return plum_int(node->i);
    case PLUM_TOML_NODE_FLOAT:
        return plum_float(node->f);
    case PLUM_TOML_NODE_BOOL:
        return plum_bool(node->b);
    case PLUM_TOML_NODE_ARRAY: {
        PlumList *list = plum_list_new(node->count);
        for (size_t i = 0; i < node->count; i++) {
            plum_list_push(list, to_value(n, node->items[i]));
        }
        return plum_obj(&list->obj);
    }
    case PLUM_TOML_NODE_TABLE:
    default: {
        PlumTable *map = plum_native_new_map(n);
        for (size_t i = 0; i < node->count; i++) {
            plum_table_put(map, plum_str_value(node->keys[i], node->key_lens[i]),
                           to_value(n, node->items[i]));
        }
        return plum_obj(&map->obj);
    }
    }
}

static bool t_parse(ARGS) {
    (void)argc;
    const PlumString *text = plum_as_string(args[0]);
    if (text->len > UINT32_MAX - 1) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "toml.parse: the text is too long");
    }
    PlumSource source = {"<toml>", text->chars, text->len};
    PlumDiagList diags;
    plum_diag_init(&diags, &source);
    PlumToml doc;
    bool ok = plum_toml_parse_full(&doc, &source, &diags);
    if (ok) {
        *result = to_value(n, doc.root);
    } else {
        const PlumDiag *d = &diags.items.items[0];
        plum_native_throw(n, PLUM_ERROR_PARSE, "toml.parse: line %u, column %u: %s",
                          (unsigned)d->span.line, (unsigned)d->span.column, d->message);
    }
    plum_toml_free(&doc);
    plum_diag_free(&diags);
    return ok;
}

/* ------------------------------------------------------------------ */
/* stringify                                                           */
/* ------------------------------------------------------------------ */

#define MAX_DEPTH 256

typedef struct Writer {
    PlumNative *n;
    Buf out;
} Writer;

static bool is_bare_key(const PlumString *k) {
    if (k->len == 0) {
        return false;
    }
    for (size_t i = 0; i < k->len; i++) {
        char c = k->chars[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

/* Is the text UTF-8, as TOML must be (a zero byte is written \u0000)? */
static bool utf8_ok(const char *s, size_t len) {
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || s[i] == '\0') {
            if (!plum_utf8_valid(s + start, i - start)) {
                return false;
            }
            start = i + 1;
        }
    }
    return true;
}

static bool put_quoted(Writer *w, const char *s, size_t len) {
    if (!utf8_ok(s, len)) {
        return plum_native_throw(w->n, PLUM_ERROR_VALUE,
                                 "toml.stringify: a string isn't valid UTF-8, which TOML must be");
    }
    plum_array_push(&w->out, '"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':
            put_str(&w->out, "\\\"");
            break;
        case '\\':
            put_str(&w->out, "\\\\");
            break;
        case '\n':
            put_str(&w->out, "\\n");
            break;
        case '\t':
            put_str(&w->out, "\\t");
            break;
        case '\r':
            put_str(&w->out, "\\r");
            break;
        case '\b':
            put_str(&w->out, "\\b");
            break;
        case '\f':
            put_str(&w->out, "\\f");
            break;
        default:
            if (c < 0x20 || c == 0x7F) {
                char esc[8];
                snprintf(esc, sizeof esc, "\\u%04X", c);
                put_str(&w->out, esc);
            } else {
                plum_array_push(&w->out, (char)c);
            }
        }
    }
    plum_array_push(&w->out, '"');
    return true;
}

static bool put_key(Writer *w, PlumValue key) {
    if (!plum_is_obj(key, PLUM_OBJ_STRING)) {
        return plum_native_throw(w->n, PLUM_ERROR_VALUE,
                                 "toml.stringify: a table's keys must be str");
    }
    const PlumString *k = plum_as_string(key);
    if (is_bare_key(k)) {
        put(&w->out, k->chars, k->len);
        return true;
    }
    return put_quoted(w, k->chars, k->len);
}

static bool is_map(PlumValue v) {
    return plum_is_obj(v, PLUM_OBJ_MAP) && !plum_as_table(v)->is_set;
}

/* A list of maps only (and at least one) is written as [[array]] of tables. */
static bool is_table_array(PlumValue v) {
    if (!plum_is_obj(v, PLUM_OBJ_LIST)) {
        return false;
    }
    const PlumList *list = plum_as_list(v);
    for (size_t i = 0; i < list->len; i++) {
        if (!is_map(list->items[i])) {
            return false;
        }
    }
    return list->len > 0;
}

static bool put_inline(Writer *w, PlumValue v, int depth) {
    if (depth > MAX_DEPTH) {
        return plum_native_throw(w->n, PLUM_ERROR_VALUE,
                                 "toml.stringify: nested more than %d deep (does a value hold "
                                 "itself?)",
                                 MAX_DEPTH);
    }
    char tmp[PLUM_FLOAT_BUF + 8];
    switch (plum_value_type(v)) {
    case PLUM_VAL_NONE:
        return plum_native_throw(w->n, PLUM_ERROR_VALUE,
                                 "toml.stringify: TOML has no null; leave the key out instead");
    case PLUM_VAL_BOOL:
        put_str(&w->out, plum_as_bool(v) ? "true" : "false");
        return true;
    case PLUM_VAL_INT:
        snprintf(tmp, sizeof tmp, "%lld", (long long)plum_as_int(v));
        put_str(&w->out, tmp);
        return true;
    case PLUM_VAL_FLOAT:
        put_str(&w->out,
                isnan(plum_as_float(v)) ? "nan" : plum_format_float(plum_as_float(v), tmp));
        return true;
    case PLUM_VAL_OBJ:
        break;
    }
    switch ((PlumObjType)plum_as_obj(v)->type) {
    case PLUM_OBJ_STRING: {
        const PlumString *s = plum_as_string(v);
        return put_quoted(w, s->chars, s->len);
    }
    case PLUM_OBJ_LIST: {
        const PlumList *list = plum_as_list(v);
        plum_array_push(&w->out, '[');
        for (size_t i = 0; i < list->len; i++) {
            if (i > 0) {
                put_str(&w->out, ", ");
            }
            if (!put_inline(w, list->items[i], depth + 1)) {
                return false;
            }
        }
        plum_array_push(&w->out, ']');
        return true;
    }
    case PLUM_OBJ_MAP: {
        const PlumTable *t = plum_as_table(v);
        if (t->is_set) {
            return plum_native_throw(w->n, PLUM_ERROR_VALUE,
                                     "toml.stringify: a set has no TOML form; make it a list");
        }
        plum_array_push(&w->out, '{');
        int64_t pos = 0;
        bool first = true;
        const PlumTableEntry *e;
        while ((e = plum_table_next(t, &pos)) != NULL) {
            put_str(&w->out, first ? " " : ", ");
            first = false;
            if (!put_key(w, e->key)) {
                return false;
            }
            put_str(&w->out, " = ");
            if (!put_inline(w, e->value, depth + 1)) {
                return false;
            }
        }
        put_str(&w->out, first ? "}" : " }");
        return true;
    }
    default:
        return plum_native_throw(w->n, PLUM_ERROR_VALUE,
                                 "toml.stringify: only maps, lists, str, int, float and bool have "
                                 "a TOML form");
    }
}

/* The header's name so far, each key as TOML writes it. */
static bool put_path(Writer *w, const PlumValue *path, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            plum_array_push(&w->out, '.');
        }
        if (!put_key(w, path[i])) {
            return false;
        }
    }
    return true;
}

typedef PLUM_ARRAY(PlumValue) Path;

/* A table: its plain values, then its tables, then its arrays of tables. */
static bool put_table(Writer *w, const PlumTable *t, Path *path, int depth) {
    if (depth > MAX_DEPTH) {
        return plum_native_throw(w->n, PLUM_ERROR_VALUE,
                                 "toml.stringify: nested more than %d deep (does a value hold "
                                 "itself?)",
                                 MAX_DEPTH);
    }
    int64_t pos = 0;
    const PlumTableEntry *e;
    while ((e = plum_table_next(t, &pos)) != NULL) {
        if (is_map(e->value) || is_table_array(e->value)) {
            continue;
        }
        if (!put_key(w, e->key)) {
            return false;
        }
        put_str(&w->out, " = ");
        if (!put_inline(w, e->value, depth + 1)) {
            return false;
        }
        plum_array_push(&w->out, '\n');
    }
    for (int pass = 0; pass < 2; pass++) {
        pos = 0;
        while ((e = plum_table_next(t, &pos)) != NULL) {
            bool table = is_map(e->value);
            if (pass == 0 ? !table : !is_table_array(e->value)) {
                continue;
            }
            plum_array_push(path, e->key);
            size_t count = table ? 1 : plum_as_list(e->value)->len;
            for (size_t i = 0; i < count; i++) {
                const PlumTable *sub =
                    plum_as_table(table ? e->value : plum_as_list(e->value)->items[i]);
                if (w->out.len > 0) {
                    plum_array_push(&w->out, '\n');
                }
                put_str(&w->out, table ? "[" : "[[");
                if (!put_path(w, path->items, path->len)) {
                    return false;
                }
                put_str(&w->out, table ? "]\n" : "]]\n");
                if (!put_table(w, sub, path, depth + 1)) {
                    return false;
                }
            }
            path->len--;
        }
    }
    return true;
}

static bool t_stringify(ARGS) {
    (void)argc;
    if (!is_map(args[0])) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "toml.stringify: a TOML document is a map (a table) at the top");
    }
    Writer w = {n, {0}};
    Path path = {0};
    bool ok = put_table(&w, plum_as_table(args[0]), &path, 0);
    plum_array_free(&path);
    if (ok) {
        *result = plum_str_value(w.out.items != NULL ? w.out.items : "", w.out.len);
    }
    plum_array_free(&w.out);
    return ok;
}

static const PlumNativeFunction functions[] = {
    {"parse", "(text: str) -> Json", t_parse,
     "TOML 1.0 text as a map: tables are maps, arrays lists; dates and times are their text; a "
     "ParseError says where it went wrong"},
    {"stringify", "(value: Json) -> str", t_stringify,
     "a map as TOML text: plain values first, then [tables] and [[arrays of tables]]"},
};

const PlumNativeModule plum_module_toml = {
    "toml",    "read and write TOML 1.0 configuration files",
    functions, sizeof functions / sizeof functions[0],
    NULL,      0,
};
