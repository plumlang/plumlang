/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "check.h"
#include "suites.h"
#include "util/arena.h"

#include <stddef.h>
#include <stdint.h>

void plum_test_arena(void) {
    PlumArena arena;
    plum_arena_init(&arena);

    /* Every allocation is aligned for any type, whatever came before. */
    int aligned = 1;
    for (size_t size = 1; size < 100; size++) {
        void *p = plum_arena_alloc(&arena, size);
        aligned = aligned && ((uintptr_t)p % PLUM_MAX_ALIGN) == 0;
    }
    CHECK(aligned);

    /* Allocations don't overlap: write a pattern, read it back. */
    unsigned char *blocks[200];
    for (int i = 0; i < 200; i++) {
        blocks[i] = plum_arena_alloc(&arena, 1000);
        memset(blocks[i], i, 1000);
    }
    int intact = 1;
    for (int i = 0; i < 200; i++) {
        intact = intact && blocks[i][0] == (unsigned char)i && blocks[i][999] == (unsigned char)i;
    }
    CHECK(intact);

    /* A large block, then small ones keep using the current chunk. */
    unsigned char *big = plum_arena_alloc(&arena, 1 << 20);
    big[(1 << 20) - 1] = 42;
    unsigned char *small = plum_arena_alloc(&arena, 8);
    small[0] = 1;
    CHECK(big[(1 << 20) - 1] == 42);

    int *zeros = plum_arena_alloc_zeroed(&arena, 32 * sizeof(int));
    CHECK(zeros[0] == 0 && zeros[31] == 0);

    char *s = plum_arena_strndup(&arena, "plant x = 5", 5);
    CHECK_STR(s, "plant");

    CHECK(arena.bytes_used > (size_t)(1 << 20));
    CHECK(arena.bytes_reserved >= arena.bytes_used);

    plum_arena_free(&arena);
    CHECK(arena.head == NULL && arena.bytes_used == 0);

    /* An arena can be reused after it is freed. */
    CHECK(plum_arena_alloc(&arena, 16) != NULL);
    plum_arena_free(&arena);
}
