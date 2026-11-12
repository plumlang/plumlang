/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "util/intern.h"

#include <string.h>

void plum_interner_init(PlumInterner *in) {
    plum_arena_init(&in->arena);
    plum_map_init(&in->table);
}

void plum_interner_free(PlumInterner *in) {
    plum_map_free(&in->table);
    plum_arena_free(&in->arena);
}

const char *plum_intern(PlumInterner *in, const char *s, size_t len) {
    void *found = NULL;
    if (plum_map_get(&in->table, s, len, &found)) {
        return found;
    }
    char *copy = plum_arena_strndup(&in->arena, s, len);
    plum_map_set(&in->table, copy, len, copy);
    return copy;
}

const char *plum_intern_cstr(PlumInterner *in, const char *s) {
    return plum_intern(in, s, strlen(s));
}

size_t plum_interner_count(const PlumInterner *in) {
    return in->table.count;
}
