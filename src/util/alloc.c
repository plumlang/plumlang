/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "util/alloc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The size of a block from malloc, for --mem-stats (0 where unknown). */
#if defined(_WIN32)
#include <malloc.h>
#define BLOCK_SIZE(p) _msize(p)
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#define BLOCK_SIZE(p) malloc_size(p)
#elif defined(__linux__)
#include <malloc.h>
#define BLOCK_SIZE(p) malloc_usable_size(p)
#else
#define BLOCK_SIZE(p)      ((void)(p), (size_t)0)
#define BLOCK_SIZE_UNKNOWN 1
#endif

/*
 * The statistics being counted into, set by plum_alloc_set_stats for the
 * length of a `plum run --mem-stats`, and NULL otherwise.
 *
 * This pointer is the one exception to "no mutable global state"
 * (AGENTS.md): every allocation in the program goes through these
 * functions, which take no context, so the only other way to reach the
 * VM's counters would be to pass a context to every allocation. The counts
 * themselves belong to whoever set the pointer (the VM's run).
 */
static PlumAllocStats *counting;

void plum_alloc_set_stats(PlumAllocStats *stats) {
    if (stats != NULL) {
        memset(stats, 0, sizeof *stats);
#ifndef BLOCK_SIZE_UNKNOWN
        stats->bytes_known = true;
#endif
    }
    counting = stats;
}

void plum_alloc_count(int i) {
    if (counting != NULL && i >= 0 && i < PLUM_ALLOC_COUNTERS) {
        counting->counts[i]++;
    }
}

/* Counting is kept out of line, so the allocation functions stay as small
 * as without it. */
#if defined(__GNUC__) || defined(__clang__)
#define COLD __attribute__((noinline, cold))
#elif defined(_MSC_VER)
#define COLD __declspec(noinline)
#else
#define COLD
#endif

static void count_bytes(PlumAllocStats *s, size_t added, size_t removed) {
    s->bytes_allocated += added > removed ? added - removed : 0;
    s->bytes_in_use += (int64_t)added - (int64_t)removed;
    if (s->bytes_in_use > s->peak_bytes) {
        s->peak_bytes = s->bytes_in_use;
    }
}

COLD static void count_new(void *p) {
    counting->allocations++;
    count_bytes(counting, BLOCK_SIZE(p), 0);
}

/* A block of `old` bytes (or none, if not `resized`) became `p`. */
COLD static void count_resize(bool resized, size_t old, void *p) {
    if (!resized) {
        counting->allocations++;
    } else {
        counting->resizes++;
    }
    count_bytes(counting, BLOCK_SIZE(p), old);
}

COLD static size_t block_size(void *p) {
    return p != NULL ? BLOCK_SIZE(p) : 0;
}

COLD static void count_free(void *p) {
    counting->frees++;
    count_bytes(counting, 0, BLOCK_SIZE(p));
}

void plum_fatal(const char *what) {
    fprintf(stderr, "plum: fatal: %s\n", what);
    fflush(stderr);
    abort();
}

static void out_of_memory(size_t size) {
    fprintf(stderr, "plum: fatal: out of memory (asked for %zu bytes)\n", size);
    fflush(stderr);
    abort();
}

size_t plum_mul_size(size_t count, size_t size) {
    if (size != 0 && count > SIZE_MAX / size) {
        plum_fatal("allocation size overflows");
    }
    return count * size;
}

size_t plum_add_size(size_t a, size_t b) {
    if (a > SIZE_MAX - b) {
        plum_fatal("allocation size overflows");
    }
    return a + b;
}

void *plum_alloc(size_t size) {
    /* malloc(0) may return NULL; always hand back a real pointer. */
    void *p = malloc(size == 0 ? 1 : size);
    if (p == NULL) {
        out_of_memory(size);
    }
    if (counting != NULL) {
        count_new(p);
    }
    return p;
}

void *plum_alloc_zeroed(size_t count, size_t size) {
    size_t total = plum_mul_size(count, size);
    void *p = calloc(total == 0 ? 1 : total, 1);
    if (p == NULL) {
        out_of_memory(total);
    }
    if (counting != NULL) {
        count_new(p);
    }
    return p;
}

void *plum_realloc(void *ptr, size_t size) {
    size_t old = counting != NULL ? block_size(ptr) : 0;
    void *p = realloc(ptr, size == 0 ? 1 : size);
    if (p == NULL) {
        out_of_memory(size);
    }
    if (counting != NULL) {
        count_resize(ptr != NULL, old, p);
    }
    return p;
}

void *plum_realloc_array(void *ptr, size_t count, size_t size) {
    return plum_realloc(ptr, plum_mul_size(count, size));
}

void plum_free(void *ptr) {
    if (counting != NULL && ptr != NULL) {
        count_free(ptr);
    }
    free(ptr);
}

char *plum_strndup(const char *s, size_t len) {
    char *copy = plum_alloc(plum_add_size(len, 1));
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

char *plum_strdup(const char *s) {
    return plum_strndup(s, strlen(s));
}
