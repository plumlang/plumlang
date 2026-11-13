/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "vm/table.h"

#include "util/alloc.h"
#include "util/random.h"

#include <string.h>

#define EMPTY   (-1)
#define REMOVED (-2)

static uint64_t hash_key(const PlumTable *t, PlumValue key) {
    unsigned char buf[9];
    switch (key.type) {
    case PLUM_VAL_INT:
        buf[0] = 'i';
        for (int i = 0; i < 8; i++) {
            buf[1 + i] = (unsigned char)((uint64_t)key.as.i >> (8 * i));
        }
        return plum_siphash(t->seed, buf, 9);
    case PLUM_VAL_BOOL:
        buf[0] = 'b';
        buf[1] = key.as.b ? 1 : 0;
        return plum_siphash(t->seed, buf, 2);
    case PLUM_VAL_OBJ:
        if (key.as.obj->type == PLUM_OBJ_STRING) {
            const PlumString *s = plum_as_string(key);
            return plum_siphash(t->seed, s->chars, s->len);
        }
        break;
    default:
        break;
    }
    return 0; /* other key types are refused by the type checker */
}

PlumTable *plum_table_new(bool is_set, const uint64_t seed[2]) {
    PlumTable *t = plum_alloc_zeroed(1, sizeof *t);
    t->obj.refcount = 1;
    t->obj.type = PLUM_OBJ_MAP;
    t->is_set = is_set;
    t->seed[0] = seed[0];
    t->seed[1] = seed[1];
    return t;
}

/* Rebuild the index for at least `want` live entries, closing gaps. */
static void rebuild(PlumTable *t, size_t want) {
    size_t live = 0;
    for (size_t i = 0; i < t->used; i++) {
        if (!t->entries[i].removed) {
            t->entries[live++] = t->entries[i];
        }
    }
    t->used = live;

    size_t size = 8;
    while (size < plum_mul_size(want, 2)) { /* index at most half full */
        size = plum_mul_size(size, 2);
    }
    plum_free(t->index);
    t->index = plum_alloc(plum_mul_size(size, sizeof(int64_t)));
    for (size_t i = 0; i < size; i++) {
        t->index[i] = EMPTY;
    }
    t->index_size = size;
    for (size_t i = 0; i < t->used; i++) {
        size_t s = (size_t)t->entries[i].hash & (size - 1);
        while (t->index[s] != EMPTY) {
            s = (s + 1) & (size - 1);
        }
        t->index[s] = (int64_t)i;
    }
}

/* The index slot holding `key`, or -1. */
static int64_t find_slot(const PlumTable *t, PlumValue key, uint64_t h) {
    if (t->index_size == 0) {
        return -1;
    }
    size_t mask = t->index_size - 1;
    for (size_t s = (size_t)h & mask;; s = (s + 1) & mask) {
        int64_t at = t->index[s];
        if (at == EMPTY) {
            return -1;
        }
        if (at >= 0) {
            const PlumTableEntry *e = &t->entries[at];
            if (e->hash == h && plum_values_equal(e->key, key)) {
                return (int64_t)s;
            }
        }
    }
}

PlumValue *plum_table_find(const PlumTable *t, PlumValue key) {
    int64_t s = find_slot(t, key, hash_key(t, key));
    return s < 0 ? NULL : &t->entries[t->index[s]].value;
}

void plum_table_put(PlumTable *t, PlumValue key, PlumValue value) {
    uint64_t h = hash_key(t, key);
    int64_t s = find_slot(t, key, h);
    if (s >= 0) {
        PlumTableEntry *e = &t->entries[t->index[s]];
        plum_release(e->value);
        e->value = value;
        plum_release(key); /* the stored key stays */
        return;
    }
    /* Keep the index at most half full, counting gaps. */
    if (plum_mul_size(t->used + 1, 2) > t->index_size) {
        rebuild(t, t->count + 1);
    }
    if (t->used == t->cap) {
        size_t cap = t->cap < 4 ? 4 : plum_mul_size(t->cap, 2);
        t->entries = plum_realloc_array(t->entries, cap, sizeof(PlumTableEntry));
        t->cap = cap;
    }
    size_t at = t->used++;
    t->entries[at] = (PlumTableEntry){key, value, h, false};
    size_t mask = t->index_size - 1;
    size_t slot = (size_t)h & mask;
    while (t->index[slot] >= 0) {
        slot = (slot + 1) & mask;
    }
    t->index[slot] = (int64_t)at;
    t->count++;
}

bool plum_table_remove(PlumTable *t, PlumValue key, PlumValue *removed) {
    int64_t s = find_slot(t, key, hash_key(t, key));
    if (s < 0) {
        return false;
    }
    PlumTableEntry *e = &t->entries[t->index[s]];
    plum_release(e->key);
    *removed = e->value;
    e->key = plum_none();
    e->value = plum_none();
    e->removed = true;
    t->index[s] = REMOVED;
    t->count--;
    return true;
}

void plum_table_clear(PlumTable *t) {
    for (size_t i = 0; i < t->used; i++) {
        plum_release(t->entries[i].key);
        plum_release(t->entries[i].value);
    }
    t->used = 0;
    t->count = 0;
    for (size_t i = 0; i < t->index_size; i++) {
        t->index[i] = EMPTY;
    }
}

PlumTable *plum_table_unique(PlumValue *slot) {
    PlumTable *t = plum_as_table(*slot);
    if (t->obj.refcount == 1) {
        return t;
    }
    PlumTable *copy = plum_table_new(t->is_set, t->seed);
    for (size_t i = 0; i < t->used; i++) {
        const PlumTableEntry *e = &t->entries[i];
        if (!e->removed) {
            plum_table_put(copy, plum_retain(e->key), plum_retain(e->value));
        }
    }
    t->obj.refcount--; /* this holder lets go of the shared one */
    *slot = plum_obj(&copy->obj);
    return copy;
}

const PlumTableEntry *plum_table_next(const PlumTable *t, int64_t *pos) {
    while (*pos >= 0 && (size_t)*pos < t->used) {
        const PlumTableEntry *e = &t->entries[(*pos)++];
        if (!e->removed) {
            return e;
        }
    }
    return NULL;
}

bool plum_tables_equal(const PlumTable *a, const PlumTable *b) {
    if (a->count != b->count || a->is_set != b->is_set) {
        return false;
    }
    for (size_t i = 0; i < a->used; i++) {
        const PlumTableEntry *e = &a->entries[i];
        if (e->removed) {
            continue;
        }
        const PlumValue *v = plum_table_find(b, e->key);
        if (v == NULL || !plum_values_equal(*v, e->value)) {
            return false;
        }
    }
    return true;
}

void plum_table_free(PlumTable *t) {
    plum_table_clear(t);
    plum_free(t->entries);
    plum_free(t->index);
    plum_free(t);
}
