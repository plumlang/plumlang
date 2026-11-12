/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "check.h"
#include "suites.h"
#include "util/alloc.h"
#include "util/map.h"

#include <stdint.h>
#include <stdio.h>

#define N 5000

void plum_test_map(void) {
    /* Known FNV-1a 64 values. */
    CHECK(plum_hash_bytes("", 0) == 14695981039346656037ULL);
    CHECK(plum_hash_bytes("a", 1) == 0xaf63dc4c8601ec8cULL);

    PlumMap map;
    plum_map_init(&map);
    CHECK(!plum_map_get(&map, "x", 1, NULL));
    CHECK(!plum_map_remove(&map, "x", 1));

    static char keys[N][16];
    for (int i = 0; i < N; i++) {
        snprintf(keys[i], sizeof keys[i], "key%d", i);
        CHECK(plum_map_set(&map, keys[i], strlen(keys[i]), (void *)(intptr_t)(i + 1)));
    }
    REQUIRE(map.count == N);

    int found = 1;
    for (int i = 0; i < N; i++) {
        void *v = NULL;
        found = found && plum_map_get(&map, keys[i], strlen(keys[i]), &v) && (intptr_t)v == i + 1;
    }
    CHECK(found);

    /* Replacing keeps the count. */
    CHECK(!plum_map_set(&map, "key7", 4, (void *)(intptr_t)700));
    void *v = NULL;
    CHECK(plum_map_get(&map, "key7", 4, &v) && (intptr_t)v == 700);
    CHECK(map.count == N);

    /* Remove every even key; odd keys survive the tombstones. */
    for (int i = 0; i < N; i += 2) {
        CHECK(plum_map_remove(&map, keys[i], strlen(keys[i])));
    }
    CHECK(map.count == N / 2);
    int odd_ok = 1, even_gone = 1;
    for (int i = 0; i < N; i++) {
        bool there = plum_map_get(&map, keys[i], strlen(keys[i]), NULL);
        if (i % 2) {
            odd_ok = odd_ok && there;
        } else {
            even_gone = even_gone && !there;
        }
    }
    CHECK(odd_ok);
    CHECK(even_gone);

    /* Iteration sees each live entry exactly once. */
    size_t cursor = 0, seen = 0;
    while (plum_map_next(&map, &cursor) != NULL) {
        seen++;
    }
    CHECK(seen == map.count);

    /* Churn: insert and remove the same key many times without growing. */
    size_t cap = map.capacity;
    for (int i = 0; i < 100000; i++) {
        plum_map_set(&map, "churn", 5, NULL);
        plum_map_remove(&map, "churn", 5);
    }
    CHECK(map.capacity <= cap * 2);
    CHECK(map.count == N / 2);

    /* Keys are compared by length and bytes, NULs included. */
    CHECK(plum_map_set(&map, "a\0b", 3, (void *)1));
    CHECK(plum_map_set(&map, "a\0c", 3, (void *)2));
    CHECK(plum_map_set(&map, "a", 1, (void *)3));
    CHECK(plum_map_get(&map, "a\0c", 3, &v) && (intptr_t)v == 2);

    plum_map_free(&map);
    CHECK(map.capacity == 0 && map.count == 0);
}
