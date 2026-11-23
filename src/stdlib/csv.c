/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The csv module: comma-separated values (RFC 4180), the format
 * spreadsheets export.
 *
 * Reading: a field in double quotes may hold the separator, line breaks
 * and "" (one quote). Lines end with \n, \r\n or \r; a last line may have
 * no line break. Empty lines are skipped. A quote inside an unquoted field
 * is just a character. Every field comes back as text.
 *
 * Writing: a field is quoted only when it must be: when it holds the
 * separator, a quote or a line break, or when it is the only field of its
 * row and empty (so the row doesn't read back as an empty line). Every
 * row ends with \n.
 */
#include "stdlib/native.h"

#include "util/array.h"
#include "vm/table.h"

#include <string.h>

#define ARGS PlumNative *n, const PlumValue *args, int argc, PlumValue *result

typedef PLUM_ARRAY(char) Buf;
typedef PLUM_ARRAY(size_t) Lines;

static void put(Buf *b, const char *s, size_t len) {
    if (len == 0) {
        return;
    }
    plum_array_reserve(b, len);
    memcpy(b->items + b->len, s, len);
    b->len += len;
}

/* The separator argument at args[at]: "," when left out, else one ASCII
 * character that isn't a quote or a line break. */
static bool get_separator(PlumNative *n, const char *fn, const PlumValue *args, int argc, int at,
                          char *sep) {
    if (argc <= at || args[at].type == PLUM_VAL_NONE) {
        *sep = ',';
        return true;
    }
    const PlumString *s = plum_as_string(args[at]);
    unsigned char c = s->len == 1 ? (unsigned char)s->chars[0] : 0;
    if (s->len != 1 || c == '"' || c == '\n' || c == '\r' || c >= 0x80) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "%s: the separator must be one character, not a quote or a line "
                                 "break: \",\", \";\" or \"\\t\", say",
                                 fn);
    }
    *sep = (char)c;
    return true;
}

static bool too_many(PlumNative *n, const char *fn) {
    return plum_native_throw(n, PLUM_ERROR_VALUE,
                             "%s: more than %zu rows or fields, the most a list can hold", fn,
                             PLUM_MAX_LIST);
}

/* Is s[i] the start of a line break? */
static bool at_line_end(const char *s, size_t len, size_t i) {
    return i < len && (s[i] == '\n' || s[i] == '\r');
}

/* Step over the line break at s[i]: \r\n, \n or \r. */
static size_t skip_line_end(const char *s, size_t len, size_t i) {
    return s[i] == '\r' && i + 1 < len && s[i + 1] == '\n' ? i + 2 : i + 1;
}

/*
 * Read every row of `text` into `rows` (lists of str), and the line each
 * row starts on into `lines`. On an error, throws and returns false;
 * the caller still owns `rows` and `lines`.
 */
static bool parse_rows(PlumNative *n, const char *fn, const PlumString *text, char sep,
                       PlumList *rows, Lines *lines) {
    const char *s = text->chars;
    size_t len = text->len, i = 0, line = 1;
    Buf field = {0};
    while (i < len) {
        if (at_line_end(s, len, i)) { /* an empty line */
            i = skip_line_end(s, len, i);
            line++;
            continue;
        }
        if (rows->len >= PLUM_MAX_LIST) {
            plum_array_free(&field);
            return too_many(n, fn);
        }
        PlumList *row = plum_list_new(0);
        size_t row_line = line;
        for (;;) { /* one field */
            field.len = 0;
            if (i < len && s[i] == '"') {
                size_t opened = line;
                i++;
                for (;;) {
                    if (i >= len) {
                        plum_release(plum_obj(&row->obj));
                        plum_array_free(&field);
                        return plum_native_throw(n, PLUM_ERROR_PARSE,
                                                 "%s: line %zu: the quoted field has no closing "
                                                 "quote",
                                                 fn, opened);
                    }
                    if (s[i] == '"') {
                        if (i + 1 < len && s[i + 1] == '"') { /* "" is one quote */
                            plum_array_push(&field, '"');
                            i += 2;
                            continue;
                        }
                        i++;
                        break;
                    }
                    if (s[i] == '\n') {
                        line++;
                    }
                    plum_array_push(&field, s[i]);
                    i++;
                }
                if (i < len && s[i] != sep && !at_line_end(s, len, i)) {
                    plum_release(plum_obj(&row->obj));
                    plum_array_free(&field);
                    return plum_native_throw(n, PLUM_ERROR_PARSE,
                                             "%s: line %zu: expected the separator or the end of "
                                             "the line after the closing quote",
                                             fn, line);
                }
            } else {
                size_t start = i;
                while (i < len && s[i] != sep && !at_line_end(s, len, i)) {
                    i++;
                }
                put(&field, s + start, i - start);
            }
            if (row->len >= PLUM_MAX_LIST) {
                plum_release(plum_obj(&row->obj));
                plum_array_free(&field);
                return too_many(n, fn);
            }
            plum_list_push(row, plum_str_value(field.items != NULL ? field.items : "", field.len));
            if (i < len && s[i] == sep) {
                i++;
                continue; /* another field, maybe empty */
            }
            if (i < len) {
                i = skip_line_end(s, len, i);
                line++;
            }
            break;
        }
        plum_list_push(rows, plum_obj(&row->obj));
        plum_array_push(lines, row_line);
    }
    plum_array_free(&field);
    return true;
}

/* parse(text, separator = ","): every row, every field as text. */
static bool csv_parse(ARGS) {
    char sep = ',';
    if (!get_separator(n, "csv.parse", args, argc, 1, &sep)) {
        return false;
    }
    PlumList *rows = plum_list_new(0);
    Lines lines = {0};
    bool ok = parse_rows(n, "csv.parse", plum_as_string(args[0]), sep, rows, &lines);
    plum_array_free(&lines);
    if (!ok) {
        plum_release(plum_obj(&rows->obj));
        return false;
    }
    *result = plum_obj(&rows->obj);
    return true;
}

/* parse_records(text, separator = ","): a map for each row after the
 * first, whose fields name the keys. */
static bool csv_parse_records(ARGS) {
    const char *fn = "csv.parse_records";
    char sep = ',';
    if (!get_separator(n, fn, args, argc, 1, &sep)) {
        return false;
    }
    PlumList *rows = plum_list_new(0);
    Lines lines = {0};
    if (!parse_rows(n, fn, plum_as_string(args[0]), sep, rows, &lines)) {
        plum_array_free(&lines);
        plum_release(plum_obj(&rows->obj));
        return false;
    }
    PlumList *out = plum_list_new(rows->len > 0 ? rows->len - 1 : 0);
    bool ok = true;
    if (rows->len > 0) {
        const PlumList *header = plum_as_list(rows->items[0]);
        for (size_t k = 0; k < header->len && ok; k++) {
            for (size_t j = 0; j < k; j++) {
                if (plum_values_equal(header->items[j], header->items[k])) {
                    const PlumString *name = plum_as_string(header->items[k]);
                    ok = plum_native_throw(n, PLUM_ERROR_VALUE,
                                           "%s: the header names the field \"%.*s\" twice "
                                           "(fields %zu and %zu)",
                                           fn, name->len > 64 ? 64 : (int)name->len, name->chars,
                                           j + 1, k + 1);
                    break;
                }
            }
        }
        for (size_t r = 1; r < rows->len && ok; r++) {
            const PlumList *row = plum_as_list(rows->items[r]);
            if (row->len > header->len) {
                ok = plum_native_throw(n, PLUM_ERROR_VALUE,
                                       "%s: line %zu has %zu fields, but the header has %zu", fn,
                                       lines.items[r], row->len, header->len);
                break;
            }
            PlumTable *map = plum_native_new_map(n);
            for (size_t k = 0; k < header->len; k++) {
                PlumValue value = k < row->len ? plum_retain(row->items[k]) : plum_str_value("", 0);
                plum_table_put(map, plum_retain(header->items[k]), value);
            }
            plum_list_push(out, plum_obj(&map->obj));
        }
    }
    plum_array_free(&lines);
    plum_release(plum_obj(&rows->obj));
    if (!ok) {
        plum_release(plum_obj(&out->obj));
        return false;
    }
    *result = plum_obj(&out->obj);
    return true;
}

/* stringify(rows, separator = ","): CSV text, each row ending with \n. */
static bool csv_stringify(ARGS) {
    const char *fn = "csv.stringify";
    char sep = ',';
    if (!get_separator(n, fn, args, argc, 1, &sep)) {
        return false;
    }
    const PlumList *rows = plum_as_list(args[0]);
    Buf b = {0};
    for (size_t r = 0; r < rows->len; r++) {
        const PlumList *row = plum_as_list(rows->items[r]);
        for (size_t k = 0; k < row->len; k++) {
            const PlumString *f = plum_as_string(row->items[k]);
            size_t quotes = 0;
            bool quote = row->len == 1 && f->len == 0;
            for (size_t i = 0; i < f->len; i++) {
                char c = f->chars[i];
                quotes += c == '"';
                quote = quote || c == sep || c == '"' || c == '\n' || c == '\r';
            }
            size_t need = (k > 0) + f->len + (quote ? quotes + 2 : 0) + 1;
            if (need > PLUM_MAX_STRING - b.len) {
                plum_array_free(&b);
                return plum_native_throw(n, PLUM_ERROR_VALUE,
                                         "%s: the text would be longer than %zu bytes, the most a "
                                         "str can hold",
                                         fn, PLUM_MAX_STRING);
            }
            if (k > 0) {
                put(&b, &sep, 1);
            }
            if (!quote) {
                put(&b, f->chars, f->len);
                continue;
            }
            plum_array_push(&b, '"');
            for (size_t i = 0; i < f->len; i++) {
                if (f->chars[i] == '"') {
                    plum_array_push(&b, '"');
                }
                plum_array_push(&b, f->chars[i]);
            }
            plum_array_push(&b, '"');
        }
        if (b.len >= PLUM_MAX_STRING) {
            plum_array_free(&b);
            return plum_native_throw(n, PLUM_ERROR_VALUE,
                                     "%s: the text would be longer than %zu bytes, the most a "
                                     "str can hold",
                                     fn, PLUM_MAX_STRING);
        }
        plum_array_push(&b, '\n');
    }
    *result = plum_str_value(b.items != NULL ? b.items : "", b.len);
    plum_array_free(&b);
    return true;
}

static const PlumNativeFunction functions[] = {
    {"parse", "(text: str, separator: str = \",\") -> list[list[str]]", csv_parse,
     "every row, as a list of its fields (text)"},
    {"parse_records", "(text: str, separator: str = \",\") -> list[map[str, str]]",
     csv_parse_records,
     "a map for each row, keyed by the first row; a short row's missing fields are \"\""},
    {"stringify", "(rows: list[list[str]], separator: str = \",\") -> str", csv_stringify,
     "the rows as CSV, fields quoted only when they must be; each row ends with \\n"},
};

const PlumNativeModule plum_module_csv = {
    "csv",     "read and write comma-separated values (RFC 4180)",
    functions, sizeof functions / sizeof functions[0],
    NULL,      0,
};
