/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The native layer: standard-library modules written in C.
 *
 * A module is a table of functions and constants. Each function has a
 * signature written like Plumlang (`(x: float, digits: int = 0) -> float`),
 * which is all the type checker and compiler need: a module is added by
 * writing its C functions and listing them, nothing else.
 *
 * In signatures, `num` means int or float: a parameter takes either, and a
 * `num` result is an int when every `num` argument is an int, else a float.
 * A parameter with `= default` may be left out; the function then sees
 * none in its place. The methods of str are a module named "str" whose
 * functions take `self: str` first.
 *
 * A native function gets its arguments (borrowed) and stores its result
 * with a reference of its own. On failure it calls plum_native_throw and
 * returns false.
 */
#ifndef PLUM_STDLIB_NATIVE_H
#define PLUM_STDLIB_NATIVE_H

#include "diag/diag.h"
#include "vm/value.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* The running program, as native functions see it. */
typedef struct PlumNative PlumNative;

typedef bool (*PlumNativeFn)(PlumNative *n, const PlumValue *args, int argc, PlumValue *result);

typedef struct PlumNativeFunction {
    const char *name;
    const char *signature;
    PlumNativeFn fn;
    const char *doc; /* one line, for `plum stdlib` */
} PlumNativeFunction;

typedef enum PlumNativeConstKind {
    PLUM_NATIVE_INT,
    PLUM_NATIVE_FLOAT,
    PLUM_NATIVE_INFINITY, /* not a constant expression everywhere (MSVC) */
} PlumNativeConstKind;

typedef struct PlumNativeConstant {
    const char *name;
    PlumNativeConstKind kind;
    int64_t i;
    double f;
    const char *doc;
} PlumNativeConstant;

typedef struct PlumNativeModule {
    const char *name;
    const char *doc;
    const PlumNativeFunction *functions;
    size_t function_count;
    const PlumNativeConstant *constants;
    size_t constant_count;
} PlumNativeModule;

/* ---- Looking modules up (compile time) ---------------------------- */

/* The standard module `name` ("math"), or NULL. */
const PlumNativeModule *plum_native_module(const char *name);
/* Every module, for listing; NULL past the end. */
const PlumNativeModule *plum_native_module_at(size_t i);
const PlumNativeFunction *plum_native_function(const PlumNativeModule *m, const char *name);
const PlumNativeConstant *plum_native_constant(const PlumNativeModule *m, const char *name);

/* A function's number for CALL_NATIVE, and back. */
int plum_native_id(const PlumNativeFunction *fn);
const PlumNativeFunction *plum_native_by_id(int id);

/* Is parameter `i` of `fn`'s signature a `num`? Is its result? */
bool plum_native_param_is_num(const PlumNativeFunction *fn, size_t i);
bool plum_native_result_is_num(const PlumNativeFunction *fn);

/* ---- What native functions can ask of the running program ---------- */

/* Throw a built-in error with a formatted message; returns false. */
PLUM_PRINTF(3, 4)
bool plum_native_throw(PlumNative *n, PlumErrorKind kind, const char *fmt, ...);

FILE *plum_native_out(PlumNative *n);
FILE *plum_native_err(PlumNative *n);

/* The text of a value, through its to_str(); NULL if that threw. */
PlumString *plum_native_text(PlumNative *n, PlumValue v);

/* 64 random bits from the program's generator, and seeding it. */
uint64_t plum_native_random(PlumNative *n);
void plum_native_seed_random(PlumNative *n, uint64_t seed);

/* The program's own command-line arguments (after the file). */
int plum_native_argc(PlumNative *n);
const char *plum_native_arg(PlumNative *n, int i);

/* ---- Helpers for writing them -------------------------------------- */

static inline double plum_num(PlumValue v) {
    return v.type == PLUM_VAL_INT ? (double)v.as.i : v.as.f;
}

static inline PlumValue plum_str_value(const char *s, size_t len) {
    return plum_obj(&plum_string_new(s, len)->obj);
}

/* The modules. */
extern const PlumNativeModule plum_module_math;
extern const PlumNativeModule plum_module_str;

#endif /* PLUM_STDLIB_NATIVE_H */
