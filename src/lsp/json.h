/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The JSON the language server speaks (RFC 8259): a reader that builds a
 * tree of values in an arena, and a writer that appends to a buffer.
 *
 * The reader is strict, limits nesting, and never fails on bad input in
 * any way but returning NULL. Strings are decoded to UTF-8 (escaped
 * surrogate pairs joined; a lone surrogate becomes U+FFFD) and may hold
 * NUL bytes, so they carry their length. The writer always produces valid
 * UTF-8: an invalid byte in a string is written as U+FFFD.
 */
#ifndef PLUM_LSP_JSON_H
#define PLUM_LSP_JSON_H

#include "util/arena.h"
#include "util/array.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum PlumJsonKind {
    PLUM_JSON_NULL,
    PLUM_JSON_BOOL,
    PLUM_JSON_NUMBER,
    PLUM_JSON_STRING,
    PLUM_JSON_ARRAY,
    PLUM_JSON_OBJECT,
} PlumJsonKind;

typedef struct PlumJson PlumJson;

typedef struct PlumJsonMember {
    const char *key; /* NUL-terminated (a key with a NUL in it is cut there) */
    PlumJson *value;
} PlumJsonMember;

struct PlumJson {
    PlumJsonKind kind;
    union {
        bool b;
        double n;
        struct {
            const char *chars; /* NUL-terminated as well */
            size_t len;
        } str;
        struct {
            PlumJson **items;
            size_t count;
        } arr;
        struct {
            PlumJsonMember *members;
            size_t count;
        } obj;
    } as;
};

/* Parse `len` bytes of JSON into `arena`; NULL if it isn't valid JSON. */
PlumJson *plum_json_parse(PlumArena *arena, const char *text, size_t len);

/* The member `key` of an object, or NULL (also when `obj` isn't one). */
const PlumJson *plum_json_get(const PlumJson *obj, const char *key);
/* A string member's text, or NULL. */
const char *plum_json_str(const PlumJson *obj, const char *key, size_t *len);
/* A number member as an integer, or `fallback`. */
long long plum_json_int(const PlumJson *obj, const char *key, long long fallback);

/* ---- Writing ------------------------------------------------------ */

#define PLUM_JSON_DEPTH 64

typedef struct PlumJsonWriter {
    PLUM_ARRAY(char) buf;
    int depth;
    bool first[PLUM_JSON_DEPTH]; /* nothing written yet in this array or object */
    bool after_key;
} PlumJsonWriter;

void plum_jw_init(PlumJsonWriter *w);
void plum_jw_free(PlumJsonWriter *w);
/* The text written so far (NUL-terminated) and its length. */
const char *plum_jw_text(PlumJsonWriter *w, size_t *len);

void plum_jw_object(PlumJsonWriter *w);
void plum_jw_object_end(PlumJsonWriter *w);
void plum_jw_array(PlumJsonWriter *w);
void plum_jw_array_end(PlumJsonWriter *w);
void plum_jw_key(PlumJsonWriter *w, const char *key);
void plum_jw_string(PlumJsonWriter *w, const char *s);
void plum_jw_stringn(PlumJsonWriter *w, const char *s, size_t len);
void plum_jw_int(PlumJsonWriter *w, long long n);
void plum_jw_bool(PlumJsonWriter *w, bool b);
void plum_jw_null(PlumJsonWriter *w);
/* A value as it was read (a request's id, echoed back). */
void plum_jw_value(PlumJsonWriter *w, const PlumJson *v);

#endif /* PLUM_LSP_JSON_H */
