/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * TOML (https://toml.io/en/v1.0.0): one parser, for plum.toml and
 * plum.lock (docs/orchard.md) and for the toml module
 * (src/stdlib/toml.c).
 *
 * plum_toml_parse reads the strict subset Orchard's files are written in:
 *
 *     # a comment
 *     [table]              tables, also dotted: [packages.colors]
 *     key = "text"         basic strings with the usual escapes
 *     key = 'literal'      literal strings
 *     key = ["a", "b"]     arrays of strings, over several lines if need be
 *
 * Keys are bare (letters, digits, '_' and '-') or quoted, one per line.
 * Anything else (numbers, booleans, inline tables, [[arrays of tables]],
 * multi-line strings, dotted keys) is an error at its line and column, as
 * is a key or table given twice. The document is flat: each value knows
 * the table it is in.
 *
 * plum_toml_parse_full reads all of TOML 1.0 into a tree of nodes, with
 * the same code for what the two share (strings, keys, comments, lines),
 * so a manifest reads the same either way. Dates and times are kept as
 * their text (with 'T' between date and time, and 'Z' in capitals).
 */
#ifndef PLUM_ORCHARD_TOML_H
#define PLUM_ORCHARD_TOML_H

#include "diag/diag.h"
#include "util/arena.h"
#include "util/array.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum PlumTomlKind {
    PLUM_TOML_STRING,
    PLUM_TOML_ARRAY,
} PlumTomlKind;

typedef struct PlumTomlValue {
    const char *table; /* "" for the top level, else "package" or "packages.colors" */
    const char *key;
    PlumTomlKind kind;
    const char *str;    /* a STRING's text, NUL-terminated (it may hold no NUL) */
    const char **items; /* an ARRAY's strings */
    size_t count;
    PlumSpan key_span;
    PlumSpan value_span;
    uint32_t last_line; /* the line the value ends on */
} PlumTomlValue;

typedef struct PlumTomlTable {
    const char *name;
    PlumSpan span;
    uint32_t last_line; /* the last line holding a key of this table (or its header) */
} PlumTomlTable;

/* A value of a full document (plum_toml_parse_full). */
typedef enum PlumTomlNodeKind {
    PLUM_TOML_NODE_STRING,
    PLUM_TOML_NODE_INTEGER,
    PLUM_TOML_NODE_FLOAT,
    PLUM_TOML_NODE_BOOL,
    PLUM_TOML_NODE_DATETIME, /* the text: 1979-05-27T07:32:00Z, 1979-05-27, 07:32:00 */
    PLUM_TOML_NODE_ARRAY,
    PLUM_TOML_NODE_TABLE,
} PlumTomlNodeKind;

typedef struct PlumTomlNode PlumTomlNode;

struct PlumTomlNode {
    PlumTomlNodeKind kind;
    const char *str; /* STRING and DATETIME: the text, NUL-terminated (a STRING may hold NUL) */
    size_t len;
    int64_t i;
    double f;
    bool b;
    const char **keys;    /* TABLE: the keys, in the order they came (NUL-terminated, but */
    size_t *key_lens;     /* a key may hold \u0000: its length is here) */
    PlumTomlNode **items; /* ARRAY: the elements; TABLE: the values of `keys` */
    size_t count;
    size_t cap;
    unsigned flags; /* how a table or array came to be (toml.c) */
    PlumSpan span;  /* where it was defined */
};

typedef struct PlumToml {
    PlumArena arena; /* every string above, and every node */
    PLUM_ARRAY(PlumTomlValue) values;
    PLUM_ARRAY(PlumTomlTable) tables;
    PlumTomlNode *root; /* plum_toml_parse_full's document: a TABLE */
} PlumToml;

/*
 * Parse `source` into `doc`; errors (E0901, E0902) go to `diags`. Returns
 * whether there were none. `doc` needs plum_toml_free either way.
 */
bool plum_toml_parse(PlumToml *doc, const PlumSource *source, PlumDiagList *diags);
/* Parse all of TOML 1.0 into doc->root, the same way; stops at the first error. */
bool plum_toml_parse_full(PlumToml *doc, const PlumSource *source, PlumDiagList *diags);
void plum_toml_free(PlumToml *doc);

/* The value of `key` in `table` ("" for the top level), or NULL. */
const PlumTomlValue *plum_toml_get(const PlumToml *doc, const char *table, const char *key);

/* The table named `name`, or NULL. */
const PlumTomlTable *plum_toml_table(const PlumToml *doc, const char *name);

/* Is `s` (of `len` bytes) valid UTF-8 with no zero byte? */
bool plum_utf8_valid(const char *s, size_t len);

#endif /* PLUM_ORCHARD_TOML_H */
