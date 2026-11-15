/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "check.h"
#include "suites.h"
#include "util/alloc.h"

#include <stdint.h>

void plum_test_alloc(void) {
    char *p = plum_alloc(0);
    CHECK(p != NULL);
    plum_free(p);

    int *zeros = plum_alloc_zeroed(64, sizeof(int));
    int all_zero = 1;
    for (int i = 0; i < 64; i++) {
        all_zero = all_zero && zeros[i] == 0;
    }
    CHECK(all_zero);

    zeros = plum_realloc_array(zeros, 1024, sizeof(int));
    zeros[1023] = 7;
    CHECK(zeros[0] == 0 && zeros[1023] == 7);
    plum_free(zeros);

    char *s = plum_strdup("plumlang");
    CHECK_STR(s, "plumlang");
    plum_free(s);

    char *part = plum_strndup("branch root", 6);
    CHECK_STR(part, "branch");
    plum_free(part);

    CHECK(plum_mul_size(0, SIZE_MAX) == 0);
    CHECK(plum_mul_size(1024, 1024) == 1048576);
    CHECK(plum_add_size(SIZE_MAX - 1, 1) == SIZE_MAX);

    /* --mem-stats: counting while a PlumAllocStats is set, and only then */
    PlumAllocStats stats;
    plum_alloc_set_stats(&stats);
    char *a = plum_alloc(100);
    char *b = plum_alloc_zeroed(10, 10);
    a = plum_realloc(a, 1000);
    plum_alloc_count(2);
    plum_alloc_count(2);
    plum_alloc_count(PLUM_ALLOC_COUNTERS); /* out of range: ignored */
    plum_free(a);
    plum_free(b);
    plum_alloc_set_stats(NULL);
    plum_free(plum_alloc(10)); /* not counted */
    CHECK(stats.allocations == 2);
    CHECK(stats.resizes == 1);
    CHECK(stats.frees == 2);
    CHECK(stats.counts[2] == 2 && stats.counts[0] == 0);
    if (stats.bytes_known) {
        CHECK(stats.bytes_in_use == 0);
        CHECK(stats.peak_bytes >= 1100);
        CHECK(stats.bytes_allocated >= 1100);
    }
}
