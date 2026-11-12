/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "util/array.h"

void *plum_array_grow(void *items, size_t *cap, size_t len, size_t extra, size_t elem_size) {
    size_t need = plum_add_size(len, extra);
    if (need <= *cap) {
        return items;
    }
    size_t grown = *cap < 8 ? 8 : *cap;
    while (grown < need) {
        grown = plum_mul_size(grown, 2);
    }
    *cap = grown;
    return plum_realloc_array(items, grown, elem_size);
}
