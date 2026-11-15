/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Checked allocation (PREAMBLE 3.4). These never return NULL: running out
 * of memory, or asking for a size that overflows size_t, prints a fatal
 * error and aborts. Code that uses them has no NULL checks to forget.
 */
#ifndef PLUM_UTIL_ALLOC_H
#define PLUM_UTIL_ALLOC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void *plum_alloc(size_t size);
void *plum_alloc_zeroed(size_t count, size_t size);
void *plum_realloc(void *ptr, size_t size);
/* Grow an array of `count` elements of `size` bytes; checks count * size. */
void *plum_realloc_array(void *ptr, size_t count, size_t size);
void plum_free(void *ptr);

char *plum_strdup(const char *s);
char *plum_strndup(const char *s, size_t len);

/* count * size, or a fatal error if it overflows. */
size_t plum_mul_size(size_t count, size_t size);
/* a + b, or a fatal error if it overflows. */
size_t plum_add_size(size_t a, size_t b);

/*
 * Memory statistics (plum run --mem-stats). Whoever wants them owns a
 * PlumAllocStats and points the allocator at it for as long as it counts;
 * the allocator keeps nothing else. When no one counts, each allocation
 * pays one test of a NULL pointer.
 *
 * Bytes are the sizes of the blocks the C library hands out (they may be
 * a little more than asked for), so frees can be counted too. Where the C
 * library can't tell a block's size, `bytes_known` is false and only the
 * counts are kept.
 */
#define PLUM_ALLOC_COUNTERS 16

typedef struct PlumAllocStats {
    uint64_t allocations; /* new blocks */
    uint64_t resizes;     /* blocks grown or shrunk */
    uint64_t frees;
    uint64_t bytes_allocated; /* in all: new blocks, and what resizes added */
    int64_t bytes_in_use;     /* allocated minus freed while counting */
    int64_t peak_bytes;       /* the most bytes_in_use reached */
    bool bytes_known;
    /* counted by the allocator's users, through plum_alloc_count: the VM
     * counts the values it makes, by kind (vm/value.h) */
    uint64_t counts[PLUM_ALLOC_COUNTERS];
} PlumAllocStats;

/* Start counting into `stats` (zeroing it), or stop with NULL. */
void plum_alloc_set_stats(PlumAllocStats *stats);
/* Add one to counter `i` of the stats being counted into, if any. */
void plum_alloc_count(int i);

/* Print "plum: fatal: <what>" and abort. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#elif defined(_MSC_VER)
__declspec(noreturn)
#endif
void plum_fatal(const char *what);

#endif /* PLUM_UTIL_ALLOC_H */
