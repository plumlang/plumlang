/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/* The time module: clocks, sleeping, and dates. */
#include "stdlib/native.h"

#include "stdlib/platform.h"
#include "vm/table.h"

#include <math.h>
#include <string.h>
#include <time.h>

#define ARGS PlumNative *n, const PlumValue *args, int argc, PlumValue *result

static bool t_now(ARGS) {
    (void)n;
    (void)args;
    (void)argc;
    *result = plum_float(plum_os_now());
    return true;
}

static bool t_clock(ARGS) {
    (void)n;
    (void)args;
    (void)argc;
    *result = plum_float(plum_os_monotonic());
    return true;
}

static bool t_sleep(ARGS) {
    (void)argc;
    double s = plum_num(args[0]);
    if (isnan(s) || s < 0 || s > 1e7) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "sleep(%g): the seconds must be 0 to 10,000,000", s);
    }
    plum_os_sleep(s);
    *result = plum_none();
    return true;
}

/* A time in seconds as a calendar date, local or UTC. */
static bool broken_down(PlumNative *n, const char *what, const PlumValue *args, int argc,
                        int utc_arg, struct tm *out) {
    double t = plum_num(args[0]);
    if (isnan(t) || t < -62135596800.0 || t > 253402300799.0) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "%s(%g): the time must be within the years 1 to 9999", what, t);
    }
    bool utc = argc > utc_arg && args[utc_arg].type == PLUM_VAL_BOOL && args[utc_arg].as.b;
    time_t secs = (time_t)floor(t);
    struct tm *tm = utc ? gmtime(&secs) : localtime(&secs);
    if (tm == NULL) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "%s(%g): this time can't be shown here", what,
                                 t);
    }
    *out = *tm;
    return true;
}

/*
 * format(t, pattern): strftime with the patterns every platform has (an
 * unknown one is a ValueError, not undefined behaviour).
 */
static bool t_format(ARGS) {
    const char *pattern = argc > 1 && args[1].type != PLUM_VAL_NONE ? plum_as_string(args[1])->chars
                                                                    : "%Y-%m-%d %H:%M:%S";
    size_t plen = strlen(pattern);
    if (plen > 200) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "format: the pattern is too long");
    }
    for (size_t i = 0; i < plen; i++) {
        if (pattern[i] == '%') {
            char c = pattern[i + 1];
            if (c == '\0' || strchr("YmdHMSyjbBaApZ%", c) == NULL) {
                return plum_native_throw(n, PLUM_ERROR_VALUE,
                                         "format: '%%%c' isn't a pattern this knows; use %%Y %%m "
                                         "%%d %%H %%M %%S %%y %%j %%b %%B %%a %%A %%p %%Z or %%%%",
                                         c == '\0' ? ' ' : c);
            }
            i++;
        }
    }
    struct tm tm;
    if (!broken_down(n, "format", args, argc, 2, &tm)) {
        return false;
    }
    char out[512];
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral" /* checked against the list above */
#endif
    size_t len = plen == 0 ? 0 : strftime(out, sizeof out, pattern, &tm);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    *result = plum_str_value(out, len);
    return true;
}

/* parts(t): {"year": 2026, "month": 11, ...} */
static bool t_parts(ARGS) {
    struct tm tm;
    if (!broken_down(n, "parts", args, argc, 1, &tm)) {
        return false;
    }
    PlumTable *map = plum_native_new_map(n);
    static const char *const names[] = {"year",   "month",  "day",     "hour",
                                        "minute", "second", "weekday", "yearday"};
    int64_t values[] = {tm.tm_year + 1900,
                        tm.tm_mon + 1,
                        tm.tm_mday,
                        tm.tm_hour,
                        tm.tm_min,
                        tm.tm_sec,
                        tm.tm_wday == 0 ? 7 : tm.tm_wday,
                        tm.tm_yday + 1};
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        plum_table_put(map, plum_str_value(names[i], strlen(names[i])), plum_int(values[i]));
    }
    *result = plum_obj(&map->obj);
    return true;
}

static const PlumNativeFunction functions[] = {
    {"now", "() -> float", t_now, "the time of day: seconds since 1970-01-01 00:00 UTC"},
    {"clock", "() -> float", t_clock,
     "seconds on a clock that never goes back, for measuring how long things take"},
    {"sleep", "(seconds: float)", t_sleep, "wait that many seconds"},
    {"format", "(t: float, pattern: str = \"%Y-%m-%d %H:%M:%S\", utc: bool = bad) -> str", t_format,
     "a time as text; local time unless utc"},
    {"parts", "(t: float, utc: bool = bad) -> map[str, int]", t_parts,
     "year, month, day, hour, minute, second, weekday (1 Monday to 7 Sunday) and yearday"},
};

const PlumNativeModule plum_module_time = {
    "time", "clocks, sleeping and dates", functions, sizeof functions / sizeof functions[0], NULL,
    0,
};
