/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "check.h"
#include "suites.h"
#include "util/intern.h"

#include <stdio.h>

void plum_test_intern(void) {
    PlumInterner in;
    plum_interner_init(&in);

    const char *a = plum_intern_cstr(&in, "branch");
    char buf[] = "branch";
    const char *b = plum_intern(&in, buf, 6);
    CHECK(a == b);
    CHECK(a != buf);
    CHECK_STR(a, "branch");

    /* A prefix is a different string. */
    const char *c = plum_intern(&in, "branches", 6);
    CHECK(c == a);
    const char *d = plum_intern_cstr(&in, "branches");
    CHECK(d != a);
    CHECK(plum_interner_count(&in) == 2);

    /* The source buffer can change afterwards. */
    buf[0] = 'X';
    CHECK_STR(a, "branch");

    /* Many distinct names, each interned twice. */
    char name[32];
    const char *first[2000];
    for (int i = 0; i < 2000; i++) {
        snprintf(name, sizeof name, "identifier_%d", i);
        first[i] = plum_intern_cstr(&in, name);
    }
    int same = 1;
    for (int i = 0; i < 2000; i++) {
        snprintf(name, sizeof name, "identifier_%d", i);
        same = same && plum_intern_cstr(&in, name) == first[i];
    }
    CHECK(same);
    CHECK(plum_interner_count(&in) == 2002);

    const char *empty = plum_intern(&in, "", 0);
    CHECK(empty != NULL && empty[0] == '\0');
    CHECK(plum_intern(&in, "", 0) == empty);

    plum_interner_free(&in);
}
