/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "check.h"
#include "suites.h"
#include "util/array.h"

typedef struct Point {
    int x, y;
} Point;

void plum_test_array(void) {
    PLUM_ARRAY(int) numbers = {0};
    CHECK(numbers.len == 0 && numbers.items == NULL);

    for (int i = 0; i < 10000; i++) {
        plum_array_push(&numbers, i * 3);
    }
    REQUIRE(numbers.len == 10000);
    CHECK(numbers.cap >= numbers.len);

    int ok = 1;
    for (int i = 0; i < 10000; i++) {
        ok = ok && numbers.items[i] == i * 3;
    }
    CHECK(ok);
    CHECK(plum_array_last(&numbers) == 9999 * 3);
    CHECK(plum_array_pop(&numbers) == 9999 * 3);
    CHECK(numbers.len == 9999);

    size_t cap = numbers.cap;
    plum_array_clear(&numbers);
    CHECK(numbers.len == 0 && numbers.cap == cap);
    plum_array_free(&numbers);
    CHECK(numbers.items == NULL && numbers.cap == 0);

    PLUM_ARRAY(Point) points = {0};
    plum_array_reserve(&points, 100);
    CHECK(points.cap >= 100 && points.len == 0);
    plum_array_push(&points, ((Point){1, 2}));
    plum_array_push(&points, ((Point){3, 4}));
    CHECK(points.items[1].x == 3 && points.items[1].y == 4);
    plum_array_free(&points);
}
