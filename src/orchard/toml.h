/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * A small, strict subset of TOML, for plum.toml and plum.lock
 * (docs/orchard.md):
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
 */
#ifndef PLUM_ORCHARD_TOML_H
#define PLUM_ORCHARD_TOML_H

#include "diag/diag.h"
#include "util/arena.h"
#include "util/array.h"

#include <stdbool.h>
#include <stddef.h>

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

typedef struct PlumToml {
    PlumArena arena; /* every string above */
    PLUM_ARRAY(PlumTomlValue) values;
    PLUM_ARRAY(PlumTomlTable) tables;
} PlumToml;

/*
 * Parse `source` into `doc`; errors (E0901, E0902) go to `diags`. Returns
 * whether there were none. `doc` needs plum_toml_free either way.
 */
bool plum_toml_parse(PlumToml *doc, const PlumSource *source, PlumDiagList *diags);
void plum_toml_free(PlumToml *doc);

/* The value of `key` in `table` ("" for the top level), or NULL. */
const PlumTomlValue *plum_toml_get(const PlumToml *doc, const char *table, const char *key);

/* The table named `name`, or NULL. */
const PlumTomlTable *plum_toml_table(const PlumToml *doc, const char *name);

/* Is `s` (of `len` bytes) valid UTF-8 with no zero byte? */
bool plum_utf8_valid(const char *s, size_t len);

#endif /* PLUM_ORCHARD_TOML_H */
