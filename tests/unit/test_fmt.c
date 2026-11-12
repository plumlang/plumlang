/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "check.h"
#include "suites.h"
#include "util/fmt.h"

#include <math.h>

void plum_test_fmt(void) {
    char buf[PLUM_FLOAT_BUF];
    CHECK_STR(plum_format_float(1.0, buf), "1.0");
    CHECK_STR(plum_format_float(0.1, buf), "0.1");
    CHECK_STR(plum_format_float(19.99, buf), "19.99");
    CHECK_STR(plum_format_float(3.5, buf), "3.5");
    CHECK_STR(plum_format_float(-2.25, buf), "-2.25");
    CHECK_STR(plum_format_float(1e20, buf), "1e+20");
    CHECK_STR(plum_format_float(6.022e23, buf), "6.022e+23");
    CHECK_STR(plum_format_float(2.5e-3, buf), "0.0025");
    CHECK_STR(plum_format_float(0.1 + 0.2, buf), "0.30000000000000004");
    CHECK_STR(plum_format_float(0.0, buf), "0.0");
    CHECK_STR(plum_format_float(-0.0, buf), "-0.0");
    CHECK_STR(plum_format_float(1e300 * 1e300, buf), "inf");
    CHECK_STR(plum_format_float(-(1e300 * 1e300), buf), "-inf");
    CHECK_STR(plum_format_float(NAN, buf), "nan");
    CHECK_STR(plum_format_float(123456789.0, buf), "123456789.0");
    CHECK_STR(plum_format_float(1e9, buf), "1000000000.0");
    CHECK_STR(plum_format_float(100.0, buf), "100.0");
    CHECK_STR(plum_format_float(1e15, buf), "1000000000000000.0");
    CHECK_STR(plum_format_float(1e16, buf), "1e+16");
    CHECK_STR(plum_format_float(1e-5, buf), "1e-05");
    CHECK_STR(plum_format_float(1.5e-7, buf), "1.5e-07");
    CHECK_STR(plum_format_float(0.0001, buf), "0.0001");
    CHECK_STR(plum_format_float(-1.25e300, buf), "-1.25e+300");
    CHECK_STR(plum_format_float(5e-324, buf), "5e-324");
    CHECK_STR(plum_format_float(1.7976931348623157e308, buf), "1.7976931348623157e+308");
    CHECK_STR(plum_format_float(1000.0005, buf), "1000.0005");
}
