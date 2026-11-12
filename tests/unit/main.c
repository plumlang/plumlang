/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/* Runs every unit-test suite; exits non-zero if any check failed. */
#include "check.h"
#include "suites.h"

#include <stdio.h>

PlumTestStats plum_test_stats;

static const struct {
    const char *name;
    void (*run)(void);
} suites[] = {
    {"alloc", plum_test_alloc},
    {"arena", plum_test_arena},
};

int main(void) {
    int failed_suites = 0;
    for (size_t i = 0; i < sizeof suites / sizeof suites[0]; i++) {
        int before = plum_test_stats.failures;
        suites[i].run();
        if (plum_test_stats.failures != before) {
            fprintf(stderr, "FAIL suite %s\n", suites[i].name);
            failed_suites++;
        }
    }
    printf("unit: %d checks, %d failed, %zu suites\n", plum_test_stats.checks,
           plum_test_stats.failures, sizeof suites / sizeof suites[0]);
    return failed_suites == 0 ? 0 : 1;
}
