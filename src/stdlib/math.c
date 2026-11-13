/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/* The math module: numbers, and the program's random numbers. */
#include "stdlib/native.h"

#include <math.h>

#define ARGS PlumNative *n, const PlumValue *args, int argc, PlumValue *result

static bool all_ints(const PlumValue *args, int argc) {
    for (int i = 0; i < argc; i++) {
        if (args[i].type != PLUM_VAL_INT) {
            return false;
        }
    }
    return true;
}

/* A float function of one float. */
#define FLOAT1(fname, expr)           \
    static bool fname(ARGS) {         \
        (void)n;                      \
        (void)argc;                   \
        double x = plum_num(args[0]); \
        *result = plum_float(expr);   \
        return true;                  \
    }

FLOAT1(m_sin, sin(x))
FLOAT1(m_cos, cos(x))
FLOAT1(m_tan, tan(x))
FLOAT1(m_atan, atan(x))
FLOAT1(m_exp, exp(x))

static bool m_asin(ARGS) {
    (void)argc;
    double x = plum_num(args[0]);
    if (x < -1.0 || x > 1.0) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "asin(%g): the value must be -1 to 1", x);
    }
    *result = plum_float(asin(x));
    return true;
}

static bool m_acos(ARGS) {
    (void)argc;
    double x = plum_num(args[0]);
    if (x < -1.0 || x > 1.0) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "acos(%g): the value must be -1 to 1", x);
    }
    *result = plum_float(acos(x));
    return true;
}

static bool m_atan2(ARGS) {
    (void)n;
    (void)argc;
    *result = plum_float(atan2(plum_num(args[0]), plum_num(args[1])));
    return true;
}

static bool m_sqrt(ARGS) {
    (void)argc;
    double x = plum_num(args[0]);
    if (x < 0) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "sqrt(%g): a negative number has no square root", x);
    }
    *result = plum_float(sqrt(x));
    return true;
}

static bool m_log(ARGS) {
    double x = plum_num(args[0]);
    if (x <= 0) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "log(%g): the value must be above 0", x);
    }
    if (argc > 1 && args[1].type != PLUM_VAL_NONE) {
        double base = plum_num(args[1]);
        if (base <= 0 || base == 1.0) {
            return plum_native_throw(n, PLUM_ERROR_VALUE,
                                     "log(%g, %g): the base must be above 0 and not 1", x, base);
        }
        *result = plum_float(log(x) / log(base));
    } else {
        *result = plum_float(log(x));
    }
    return true;
}

static bool m_abs(ARGS) {
    (void)argc;
    if (args[0].type == PLUM_VAL_INT) {
        if (args[0].as.i == INT64_MIN) {
            return plum_native_throw(n, PLUM_ERROR_OVERFLOW, "abs(%lld) doesn't fit in an int",
                                     (long long)args[0].as.i);
        }
        *result = plum_int(args[0].as.i < 0 ? -args[0].as.i : args[0].as.i);
    } else {
        *result = plum_float(fabs(args[0].as.f));
    }
    return true;
}

/* min, max: ints stay ints; with a float among them the result is a float. */
static bool m_min(ARGS) {
    (void)n;
    if (all_ints(args, argc)) {
        *result = plum_int(args[0].as.i < args[1].as.i ? args[0].as.i : args[1].as.i);
    } else {
        double a = plum_num(args[0]), b = plum_num(args[1]);
        *result = plum_float(a < b ? a : b);
    }
    return true;
}

static bool m_max(ARGS) {
    (void)n;
    if (all_ints(args, argc)) {
        *result = plum_int(args[0].as.i > args[1].as.i ? args[0].as.i : args[1].as.i);
    } else {
        double a = plum_num(args[0]), b = plum_num(args[1]);
        *result = plum_float(a > b ? a : b);
    }
    return true;
}

static bool m_clamp(ARGS) {
    if (all_ints(args, argc)) {
        int64_t x = args[0].as.i, lo = args[1].as.i, hi = args[2].as.i;
        if (lo > hi) {
            return plum_native_throw(n, PLUM_ERROR_VALUE, "clamp: low %lld is above high %lld",
                                     (long long)lo, (long long)hi);
        }
        *result = plum_int(x < lo ? lo : x > hi ? hi : x);
    } else {
        double x = plum_num(args[0]), lo = plum_num(args[1]), hi = plum_num(args[2]);
        if (lo > hi) {
            return plum_native_throw(n, PLUM_ERROR_VALUE, "clamp: low %g is above high %g", lo, hi);
        }
        *result = plum_float(x < lo ? lo : x > hi ? hi : x);
    }
    return true;
}

/* A float rounded some way, as an int: the result must fit. */
static bool to_int(PlumNative *n, const char *what, double x, double rounded, PlumValue *result) {
    if (isnan(rounded) || rounded >= 9223372036854775808.0 || rounded < -9223372036854775808.0) {
        return plum_native_throw(n, PLUM_ERROR_OVERFLOW, "%s(%g) doesn't fit in an int", what, x);
    }
    *result = plum_int((int64_t)rounded);
    return true;
}

static bool m_floor(ARGS) {
    (void)argc;
    double x = plum_num(args[0]);
    return to_int(n, "floor", x, floor(x), result);
}

static bool m_ceil(ARGS) {
    (void)argc;
    double x = plum_num(args[0]);
    return to_int(n, "ceil", x, ceil(x), result);
}

/* round(x) halves away from zero; round(x, digits) keeps that many decimals. */
static bool m_round(ARGS) {
    double x = plum_num(args[0]);
    if (argc < 2 || args[1].type == PLUM_VAL_NONE) {
        *result = plum_float(round(x));
        return true;
    }
    int64_t digits = args[1].as.i;
    if (digits < -15 || digits > 15) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "round: digits must be -15 to 15, not %lld",
                                 (long long)digits);
    }
    double scale = pow(10.0, (double)digits);
    *result = plum_float(round(x * scale) / scale);
    return true;
}

static bool m_is_nan(ARGS) {
    (void)n;
    (void)argc;
    *result = plum_bool(isnan(plum_num(args[0])));
    return true;
}

static bool m_is_finite(ARGS) {
    (void)n;
    (void)argc;
    *result = plum_bool(isfinite(plum_num(args[0])));
    return true;
}

/* ---- Random numbers ---- */

static bool m_random(ARGS) {
    (void)args;
    (void)argc;
    /* 53 random bits: every float in [0, 1) a double can hold evenly */
    *result = plum_float((double)(plum_native_random(n) >> 11) * (1.0 / 9007199254740992.0));
    return true;
}

static bool m_random_int(ARGS) {
    (void)argc;
    int64_t low = args[0].as.i, high = args[1].as.i;
    if (low > high) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "random_int: low %lld is above high %lld",
                                 (long long)low, (long long)high);
    }
    uint64_t span = (uint64_t)high - (uint64_t)low + 1; /* 0 means every int */
    uint64_t r = plum_native_random(n);
    if (span != 0) {
        /* reject the uneven top part, so every value is equally likely */
        uint64_t limit = UINT64_MAX - UINT64_MAX % span;
        while (r >= limit) {
            r = plum_native_random(n);
        }
        r %= span;
    }
    *result = plum_int((int64_t)((uint64_t)low + r));
    return true;
}

static bool m_seed(ARGS) {
    (void)argc;
    plum_native_seed_random(n, (uint64_t)args[0].as.i);
    *result = plum_none();
    return true;
}

static const PlumNativeFunction functions[] = {
    {"abs", "(x: num) -> num", m_abs, "the value without its sign"},
    {"min", "(a: num, b: num) -> num", m_min, "the smaller of two numbers"},
    {"max", "(a: num, b: num) -> num", m_max, "the larger of two numbers"},
    {"clamp", "(x: num, low: num, high: num) -> num", m_clamp, "x kept within low to high"},
    {"floor", "(x: float) -> int", m_floor, "the int at or below x"},
    {"ceil", "(x: float) -> int", m_ceil, "the int at or above x"},
    {"round", "(x: float, digits: int = 0) -> float", m_round,
     "x rounded to that many decimals; halves go away from zero"},
    {"sqrt", "(x: float) -> float", m_sqrt, "the square root"},
    {"exp", "(x: float) -> float", m_exp, "e to the power x"},
    {"log", "(x: float, base: float = E) -> float", m_log, "the logarithm, natural by default"},
    {"sin", "(x: float) -> float", m_sin, "sine of an angle in radians"},
    {"cos", "(x: float) -> float", m_cos, "cosine of an angle in radians"},
    {"tan", "(x: float) -> float", m_tan, "tangent of an angle in radians"},
    {"asin", "(x: float) -> float", m_asin, "the angle whose sine is x"},
    {"acos", "(x: float) -> float", m_acos, "the angle whose cosine is x"},
    {"atan", "(x: float) -> float", m_atan, "the angle whose tangent is x"},
    {"atan2", "(y: float, x: float) -> float", m_atan2, "the angle of the point (x, y)"},
    {"is_nan", "(x: float) -> bool", m_is_nan, "is x not a number (NAN)?"},
    {"is_finite", "(x: float) -> bool", m_is_finite, "is x neither infinite nor NAN?"},
    {"random", "() -> float", m_random, "a random float from 0 up to (not including) 1"},
    {"random_int", "(low: int, high: int) -> int", m_random_int,
     "a random int from low to high, both included"},
    {"seed_random", "(value: int)", m_seed, "restart the random numbers, so they repeat"},
};

static const PlumNativeConstant constants[] = {
    {"PI", PLUM_NATIVE_FLOAT, 0, 3.14159265358979323846,
     "the ratio of a circle's circumference to its diameter"},
    {"TAU", PLUM_NATIVE_FLOAT, 0, 6.28318530717958647692, "2 * PI, a full turn in radians"},
    {"E", PLUM_NATIVE_FLOAT, 0, 2.71828182845904523536, "the base of natural logarithms"},
    {"INF", PLUM_NATIVE_INFINITY, 0, 0, "positive infinity"},
    {"MAX_INT", PLUM_NATIVE_INT, INT64_MAX, 0, "the largest int"},
    {"MIN_INT", PLUM_NATIVE_INT, INT64_MIN, 0, "the smallest int"},
};

const PlumNativeModule plum_module_math = {
    "math",    "constants and functions on numbers, and random numbers",
    functions, sizeof functions / sizeof functions[0],
    constants, sizeof constants / sizeof constants[0],
};
