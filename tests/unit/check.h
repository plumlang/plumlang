/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * A minimal unit-test framework: CHECK records a failure and carries on,
 * REQUIRE stops the current test. Each suite is a function listed in
 * suites.h and run from main.c.
 */
#ifndef PLUM_TEST_CHECK_H
#define PLUM_TEST_CHECK_H

#include <stdio.h>
#include <string.h>

typedef struct PlumTestStats {
    int checks;
    int failures;
} PlumTestStats;

extern PlumTestStats plum_test_stats;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        plum_test_stats.checks++;                                                    \
        if (!(cond)) {                                                               \
            plum_test_stats.failures++;                                              \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

#define REQUIRE(cond)                                                                      \
    do {                                                                                   \
        plum_test_stats.checks++;                                                          \
        if (!(cond)) {                                                                     \
            plum_test_stats.failures++;                                                    \
            fprintf(stderr, "%s:%d: requirement failed: %s\n", __FILE__, __LINE__, #cond); \
            return;                                                                        \
        }                                                                                  \
    } while (0)

#define CHECK_STR(a, b) CHECK(strcmp((a), (b)) == 0)

#endif /* PLUM_TEST_CHECK_H */
