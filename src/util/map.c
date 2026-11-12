/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "util/map.h"

#include "util/alloc.h"

#include <string.h>

#define PLUM_MAP_MIN_CAPACITY 16

uint64_t plum_hash_bytes(const void *data, size_t len) {
    const unsigned char *p = data;
    uint64_t h = 14695981039346656037ULL; /* FNV-1a 64-bit offset basis */
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL; /* FNV prime */
    }
    return h;
}

void plum_map_init(PlumMap *map) {
    map->entries = NULL;
    map->capacity = 0;
    map->count = 0;
    map->used = 0;
}

void plum_map_free(PlumMap *map) {
    plum_free(map->entries);
    plum_map_init(map);
}

static bool same_key(const PlumMapEntry *e, const char *key, size_t len, uint64_t hash) {
    return e->hash == hash && e->len == len && memcmp(e->key, key, len) == 0;
}

/*
 * The slot holding `key`, or the slot where it should go: the first
 * tombstone on its probe chain if there was one, else the empty slot that
 * ended the chain. The table always has at least one empty slot.
 */
static PlumMapEntry *find_slot(PlumMapEntry *entries, size_t capacity, const char *key, size_t len,
                               uint64_t hash) {
    size_t mask = capacity - 1;
    size_t i = (size_t)hash & mask;
    PlumMapEntry *tombstone = NULL;
    for (;;) {
        PlumMapEntry *e = &entries[i];
        if (e->key == NULL) {
            return tombstone != NULL ? tombstone : e;
        }
        if (e->deleted) {
            if (tombstone == NULL) {
                tombstone = e;
            }
        } else if (same_key(e, key, len, hash)) {
            return e;
        }
        i = (i + 1) & mask;
    }
}

static void resize(PlumMap *map, size_t capacity) {
    PlumMapEntry *entries = plum_alloc_zeroed(capacity, sizeof(PlumMapEntry));
    for (size_t i = 0; i < map->capacity; i++) {
        PlumMapEntry *old = &map->entries[i];
        if (old->key != NULL && !old->deleted) {
            *find_slot(entries, capacity, old->key, old->len, old->hash) = *old;
        }
    }
    plum_free(map->entries);
    map->entries = entries;
    map->capacity = capacity;
    map->used = map->count; /* tombstones are gone */
}

bool plum_map_get(const PlumMap *map, const char *key, size_t len, void **value) {
    if (map->count == 0) {
        return false;
    }
    PlumMapEntry *e = find_slot(map->entries, map->capacity, key, len, plum_hash_bytes(key, len));
    if (e->key == NULL || e->deleted) {
        return false;
    }
    if (value != NULL) {
        *value = e->value;
    }
    return true;
}

bool plum_map_set(PlumMap *map, const char *key, size_t len, void *value) {
    /* Keep live entries + tombstones at most 3/4 of the table. */
    if (plum_mul_size(plum_add_size(map->used, 1), 4) > plum_mul_size(map->capacity, 3)) {
        size_t capacity =
            map->capacity < PLUM_MAP_MIN_CAPACITY ? PLUM_MAP_MIN_CAPACITY : map->capacity;
        /*
         * Size the new table so live entries fill at most half of it. A
         * table full of tombstones is rebuilt at the same size; either way
         * a quarter of the table can be inserted before the next rebuild.
         */
        while (plum_mul_size(plum_add_size(map->count, 1), 2) > capacity) {
            capacity = plum_mul_size(capacity, 2);
        }
        resize(map, capacity);
    }

    uint64_t hash = plum_hash_bytes(key, len);
    PlumMapEntry *e = find_slot(map->entries, map->capacity, key, len, hash);
    if (e->key != NULL && !e->deleted) {
        e->value = value;
        return false;
    }
    if (e->key == NULL) {
        map->used++; /* a reused tombstone was already counted */
    }
    e->key = key;
    e->len = len;
    e->hash = hash;
    e->value = value;
    e->deleted = false;
    map->count++;
    return true;
}

bool plum_map_remove(PlumMap *map, const char *key, size_t len) {
    if (map->count == 0) {
        return false;
    }
    PlumMapEntry *e = find_slot(map->entries, map->capacity, key, len, plum_hash_bytes(key, len));
    if (e->key == NULL || e->deleted) {
        return false;
    }
    e->deleted = true;
    e->value = NULL;
    map->count--;
    return true;
}

const PlumMapEntry *plum_map_next(const PlumMap *map, size_t *cursor) {
    while (*cursor < map->capacity) {
        const PlumMapEntry *e = &map->entries[(*cursor)++];
        if (e->key != NULL && !e->deleted) {
            return e;
        }
    }
    return NULL;
}
