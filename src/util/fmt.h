/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/* Turning values into text the way Plumlang prints them (spec 3.2.1). */
#ifndef PLUM_UTIL_FMT_H
#define PLUM_UTIL_FMT_H

#include <stddef.h>
#include <stdio.h>

/* Room for any float: sign, 17 digits, point, exponent, NUL. */
#define PLUM_FLOAT_BUF 32

/*
 * The shortest text that reads back as the same double, always with a
 * '.' or an exponent: 1.0, 0.1, 19.99, 1e+20, inf, -inf, nan.
 * Returns `buf`.
 */
char *plum_format_float(double value, char buf[PLUM_FLOAT_BUF]);

/* Write `len` bytes of text as a double-quoted literal with escapes. */
void plum_write_quoted(FILE *out, const char *text, size_t len);

#endif /* PLUM_UTIL_FMT_H */
