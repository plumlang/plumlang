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
 * '.' or an exponent. Exponents from -4 to 15 are written out in full
 * (0.0025, 1000000000.0), others in scientific notation (1e+20, 1e-05),
 * as in Python: inf, -inf and nan as they are.
 * Returns `buf`.
 */
char *plum_format_float(double value, char buf[PLUM_FLOAT_BUF]);

/* Write `len` bytes of text as a double-quoted literal with escapes. */
void plum_write_quoted(FILE *out, const char *text, size_t len);

#endif /* PLUM_UTIL_FMT_H */
