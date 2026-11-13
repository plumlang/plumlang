/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "vm/text.h"

#include "util/array.h"
#include "util/fmt.h"
#include "vm/table.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef PLUM_ARRAY(char) Buf;

static void put(Buf *b, const char *s, size_t n) {
    plum_array_reserve(b, n);
    memcpy(b->items + b->len, s, n);
    b->len += n;
}

size_t plum_utf8_length(const char *s, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            n++; /* count every byte that starts a character */
        }
    }
    return n;
}

/* Bytes taken by the first `chars` characters of s[0..len). */
static size_t utf8_prefix(const char *s, size_t len, size_t chars) {
    size_t i = 0, seen = 0;
    while (i < len) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            if (seen == chars) {
                break;
            }
            seen++;
        }
        i++;
    }
    return i;
}

typedef struct Spec {
    char fill[5]; /* one UTF-8 character */
    char align;   /* 0, '<', '>', '^' */
    long width;
    long precision; /* -1: none */
    char type;      /* 0 or one of d f e x X o b */
} Spec;

static bool parse_spec(const char *f, Spec *spec, char *error, size_t error_size) {
    memset(spec, 0, sizeof *spec);
    strcpy(spec->fill, " ");
    spec->precision = -1;
    if (f == NULL) {
        return true;
    }
    size_t n = strlen(f), i = 0;
    /* [fill] align: the fill is one character, possibly multi-byte. */
    size_t first = n > 0 ? utf8_prefix(f, n, 1) : 0;
    if (first < n && strchr("<>^", f[first]) != NULL && first <= 4) {
        memcpy(spec->fill, f, first);
        spec->fill[first] = '\0';
        spec->align = f[first];
        i = first + 1;
    } else if (n > 0 && strchr("<>^", f[0]) != NULL) {
        spec->align = f[0];
        i = 1;
    }
    while (i < n && f[i] >= '0' && f[i] <= '9') {
        spec->width = spec->width * 10 + (f[i++] - '0');
        if (spec->width > 4096) {
            snprintf(error, error_size, "the width in \"%s\" is too large (at most 4096)", f);
            return false;
        }
    }
    if (i < n && f[i] == '.') {
        i++;
        spec->precision = 0;
        if (i >= n || f[i] < '0' || f[i] > '9') {
            snprintf(error, error_size, "\"%s\" needs digits after '.'", f);
            return false;
        }
        while (i < n && f[i] >= '0' && f[i] <= '9') {
            spec->precision = spec->precision * 10 + (f[i++] - '0');
            if (spec->precision > 100) {
                snprintf(error, error_size, "the precision in \"%s\" is too large (at most 100)",
                         f);
                return false;
            }
        }
    }
    if (i < n && strchr("dfexXob", f[i]) != NULL) {
        spec->type = f[i++];
    }
    if (i != n) {
        snprintf(error, error_size, "\"%s\" is not a format; formats look like .2, >8, <10, x or b",
                 f);
        return false;
    }
    return true;
}

/* Digits of |v| in `base`, with a '-' for negatives. */
static void put_int_base(Buf *b, int64_t v, int base, bool upper) {
    char digits[72];
    size_t n = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        digits[n++] = set[u % (uint64_t)base];
        u /= (uint64_t)base;
    } while (u != 0);
    if (v < 0) {
        put(b, "-", 1);
    }
    while (n > 0) {
        put(b, &digits[--n], 1);
    }
}

static bool put_number(Buf *b, PlumValue v, const Spec *spec, char *error, size_t error_size) {
    char tmp[512];
    bool is_int = v.type == PLUM_VAL_INT;
    switch (spec->type) {
    case 'x':
    case 'X':
    case 'o':
    case 'b':
        if (!is_int) {
            snprintf(error, error_size, "format '%c' needs an int", spec->type);
            return false;
        }
        put_int_base(b, v.as.i,
                     spec->type == 'b'   ? 2
                     : spec->type == 'o' ? 8
                                         : 16,
                     spec->type == 'X');
        return true;
    case 'd':
        if (!is_int) {
            snprintf(error, error_size, "format 'd' needs an int; use .0 to round a float");
            return false;
        }
        put_int_base(b, v.as.i, 10, false);
        return true;
    default:
        break;
    }
    double f = is_int ? (double)v.as.i : v.as.f;
    if (spec->type == 'e') {
        snprintf(tmp, sizeof tmp, "%.*e", spec->precision >= 0 ? (int)spec->precision : 6, f);
    } else if (spec->type == 'f' || spec->precision >= 0) {
        snprintf(tmp, sizeof tmp, "%.*f", spec->precision >= 0 ? (int)spec->precision : 6, f);
    } else if (is_int) {
        snprintf(tmp, sizeof tmp, "%lld", (long long)v.as.i);
    } else {
        plum_format_float(f, tmp);
    }
    put(b, tmp, strlen(tmp));
    return true;
}

/* The plain text of a value; strings inside collections are quoted. */
static void put_plain(Buf *b, PlumValue v, bool quote_strings) {
    char tmp[PLUM_FLOAT_BUF + 160];
    switch (v.type) {
    case PLUM_VAL_NONE:
        put(b, "none", 4);
        return;
    case PLUM_VAL_BOOL:
        put(b, v.as.b ? "good" : "bad", v.as.b ? 4 : 3);
        return;
    case PLUM_VAL_INT:
        snprintf(tmp, sizeof tmp, "%lld", (long long)v.as.i);
        put(b, tmp, strlen(tmp));
        return;
    case PLUM_VAL_FLOAT:
        plum_format_float(v.as.f, tmp);
        put(b, tmp, strlen(tmp));
        return;
    case PLUM_VAL_OBJ:
        break;
    }
    switch ((PlumObjType)v.as.obj->type) {
    case PLUM_OBJ_STRING: {
        PlumString *s = plum_as_string(v);
        if (!quote_strings) {
            put(b, s->chars, s->len);
            return;
        }
        /* The same escapes as a string literal in source. */
        put(b, "\"", 1);
        for (size_t i = 0; i < s->len; i++) {
            unsigned char c = (unsigned char)s->chars[i];
            const char *esc = c == '\n'   ? "\\n"
                              : c == '\t' ? "\\t"
                              : c == '\r' ? "\\r"
                              : c == '"'  ? "\\\""
                              : c == '\\' ? "\\\\"
                              : c == '{'  ? "\\{"
                              : c == '}'  ? "\\}"
                                          : NULL;
            if (esc != NULL) {
                put(b, esc, strlen(esc));
            } else if (c < 0x20) {
                snprintf(tmp, sizeof tmp, "\\u{%X}", (unsigned)c);
                put(b, tmp, strlen(tmp));
            } else {
                put(b, (const char *)&s->chars[i], 1);
            }
        }
        put(b, "\"", 1);
        return;
    }
    case PLUM_OBJ_FUNCTION:
    case PLUM_OBJ_CLOSURE: {
        const PlumFunction *fn =
            v.as.obj->type == PLUM_OBJ_CLOSURE ? plum_as_closure(v)->fn : plum_as_function(v);
        snprintf(tmp, sizeof tmp, "<branch %.150s>", fn->name);
        put(b, tmp, strlen(tmp));
        return;
    }
    case PLUM_OBJ_CELL:
        put_plain(b, plum_as_cell(v)->value, quote_strings); /* never seen by programs */
        return;
    case PLUM_OBJ_OBJECT:
        /* to_str() (trait Show) arrives with methods; until then, the tree's name */
        snprintf(tmp, sizeof tmp, "<%.150s>", plum_as_object(v)->tree->name);
        put(b, tmp, strlen(tmp));
        return;
    case PLUM_OBJ_TREE_INFO:
        snprintf(tmp, sizeof tmp, "<tree %.150s>", plum_as_tree_info(v)->name);
        put(b, tmp, strlen(tmp));
        return;
    case PLUM_OBJ_LIST: {
        PlumList *list = plum_as_list(v);
        put(b, "[", 1);
        for (size_t i = 0; i < list->len; i++) {
            if (i > 0) {
                put(b, ", ", 2);
            }
            put_plain(b, list->items[i], true);
        }
        put(b, "]", 1);
        return;
    }
    case PLUM_OBJ_MAP: {
        /* {"a": 1} and {:} for maps, set([1, 2]) for sets, in insertion order */
        PlumTable *t = plum_as_table(v);
        if (!t->is_set && t->count == 0) {
            put(b, "{:}", 3);
            return;
        }
        put(b, t->is_set ? "set([" : "{", t->is_set ? 5 : 1);
        int64_t pos = 0;
        bool first = true;
        const PlumTableEntry *e;
        while ((e = plum_table_next(t, &pos)) != NULL) {
            if (!first) {
                put(b, ", ", 2);
            }
            first = false;
            put_plain(b, e->key, true);
            if (!t->is_set) {
                put(b, ": ", 2);
                put_plain(b, e->value, true);
            }
        }
        put(b, t->is_set ? "])" : "}", t->is_set ? 2 : 1);
        return;
    }
    }
}

PlumString *plum_value_text(PlumValue v, const char *format, char *error, size_t error_size) {
    Spec spec;
    if (format != NULL && format[0] == '\0') {
        format = NULL;
    }
    if (!parse_spec(format, &spec, error, error_size)) {
        return NULL;
    }
    Buf b = {0};
    bool number = v.type == PLUM_VAL_INT || v.type == PLUM_VAL_FLOAT;

    if (number) {
        if (!put_number(&b, v, &spec, error, error_size)) {
            plum_array_free(&b);
            return NULL;
        }
    } else {
        if (spec.type != 0) {
            snprintf(error, error_size, "format '%c' needs a number", spec.type);
            return NULL;
        }
        if (plum_is_obj(v, PLUM_OBJ_STRING) && spec.precision >= 0) {
            PlumString *s = plum_as_string(v);
            put(&b, s->chars, utf8_prefix(s->chars, s->len, (size_t)spec.precision));
        } else {
            put_plain(&b, v, false);
        }
    }

    /* Pad to the width with the fill character. */
    size_t chars = plum_utf8_length(b.items ? b.items : "", b.len);
    size_t width = spec.width > 0 ? (size_t)spec.width : 0;
    if (chars >= width) {
        PlumString *s = plum_string_new(b.items ? b.items : "", b.len);
        plum_array_free(&b);
        return s;
    }
    size_t pad = width - chars;
    char align = spec.align != 0 ? spec.align : number ? '>' : '<';
    size_t left = align == '>' ? pad : align == '^' ? pad / 2 : 0;
    size_t right = pad - left;
    size_t fill_len = strlen(spec.fill);
    Buf out = {0};
    for (size_t i = 0; i < left; i++) {
        put(&out, spec.fill, fill_len);
    }
    put(&out, b.items, b.len);
    for (size_t i = 0; i < right; i++) {
        put(&out, spec.fill, fill_len);
    }
    PlumString *s = plum_string_new(out.items, out.len);
    plum_array_free(&b);
    plum_array_free(&out);
    return s;
}
