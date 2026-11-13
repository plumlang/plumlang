/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Maps and sets at run time (spec 3.4): a hash table that remembers
 * insertion order. Entries sit in an array in the order they were added;
 * a separate index of positions finds them by key. Removing an entry
 * leaves a gap that the next rebuild closes.
 *
 * Keys are hashed with SipHash under the table's random key (util/random.h).
 * A set is a table whose values are all none.
 *
 * Like lists, tables are values: plum_table_unique copies a shared table
 * before a change.
 */
#ifndef PLUM_VM_TABLE_H
#define PLUM_VM_TABLE_H

#include "vm/value.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct PlumTableEntry {
    PlumValue key; /* PLUM_VAL_NONE with `removed`: a gap */
    PlumValue value;
    uint64_t hash;
    bool removed;
} PlumTableEntry;

typedef struct PlumTable {
    PlumObj obj;
    bool is_set;
    uint64_t seed[2];
    PlumTableEntry *entries; /* in insertion order */
    size_t used;             /* entries, gaps included */
    size_t cap;
    size_t count;      /* live entries */
    int64_t *index;    /* positions into entries; -1 empty, -2 removed */
    size_t index_size; /* zero or a power of two */
} PlumTable;

static inline PlumTable *plum_as_table(PlumValue v) {
    return (PlumTable *)v.as.obj;
}

PlumTable *plum_table_new(bool is_set, const uint64_t seed[2]);

/* The value stored for `key`, or NULL. */
PlumValue *plum_table_find(const PlumTable *t, PlumValue key);

/* Store key -> value, taking over both references; replaces an old value. */
void plum_table_put(PlumTable *t, PlumValue key, PlumValue value);

/* Remove `key`; on success its value's reference moves to *removed. */
bool plum_table_remove(PlumTable *t, PlumValue key, PlumValue *removed);

void plum_table_clear(PlumTable *t);

/* The table in `*slot`, copied first if shared (copy-on-write). */
PlumTable *plum_table_unique(PlumValue *slot);

/*
 * Iterate in insertion order: start with *pos = 0; each call gives the next
 * live entry (borrowed) or NULL at the end.
 */
const PlumTableEntry *plum_table_next(const PlumTable *t, int64_t *pos);

/* Same keys with equal values, in any order. */
bool plum_tables_equal(const PlumTable *a, const PlumTable *b);

void plum_table_free(PlumTable *t);

#endif /* PLUM_VM_TABLE_H */
