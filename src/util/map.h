/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * String-keyed hash map with open addressing and linear probing, for the
 * compiler and runtime internals: symbol tables, module caches, the
 * string interner.
 *
 * Keys are borrowed: the map stores the pointer and length, so a key must
 * outlive its entry (an arena or interned string does). Keys may contain
 * NUL bytes; they are compared by length and bytes.
 *
 * The hash is FNV-1a, which is fine for names from source code. Maps that
 * hold data from outside the program (the language's own map type, HTTP
 * headers) must use a randomly seeded hash instead, so a peer cannot
 * choose colliding keys (Stage 5).
 */
#ifndef PLUM_UTIL_MAP_H
#define PLUM_UTIL_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct PlumMapEntry {
    const char *key; /* NULL: never used */
    size_t len;
    uint64_t hash;
    void *value;
    bool deleted; /* tombstone: keeps probe chains intact */
} PlumMapEntry;

typedef struct PlumMap {
    PlumMapEntry *entries;
    size_t capacity; /* zero or a power of two */
    size_t count;    /* live entries */
    size_t used;     /* live entries + tombstones */
} PlumMap;

uint64_t plum_hash_bytes(const void *data, size_t len);

void plum_map_init(PlumMap *map);
void plum_map_free(PlumMap *map);

/* Find a key; on success store its value in *value (if not NULL). */
bool plum_map_get(const PlumMap *map, const char *key, size_t len, void **value);
/* Insert or replace; returns true if the key was new. */
bool plum_map_set(PlumMap *map, const char *key, size_t len, void *value);
/* Remove a key; returns true if it was there. */
bool plum_map_remove(PlumMap *map, const char *key, size_t len);

/*
 * Iterate: start with *cursor = 0; each call returns the next live entry,
 * or NULL at the end. The map must not change during iteration.
 */
const PlumMapEntry *plum_map_next(const PlumMap *map, size_t *cursor);

#endif /* PLUM_UTIL_MAP_H */
