/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The compiler turns a checked program into bytecode. It runs only on a
 * program with no errors, so every name is resolved and every expression
 * has a type; it chooses typed instructions from those types and inserts
 * the int-to-float conversion wherever the checker allowed one.
 *
 * Features still to come in Stage 5 (methods, kinds, match, modules,
 * tasks) are reported as not supported yet (E0601).
 */
#ifndef PLUM_COMPILER_COMPILER_H
#define PLUM_COMPILER_COMPILER_H

#include "diag/diag.h"
#include "parser/ast.h"
#include "util/intern.h"
#include "vm/value.h"

typedef struct PlumCompiled {
    PlumFunction *main; /* the top level; NULL if there were errors */
    int global_count;   /* module variables and functions */
    /* The built-in error trees, for the errors the VM itself raises. */
    PlumTreeInfo *errors[PLUM_ERROR_COUNT];
    PlumFunction **methods; /* every tree's methods; trees' tables point into these */
    int method_count;
    int to_str; /* the method number of to_str, which printing an object calls */
} PlumCompiled;

PlumCompiled plum_compile(const PlumProgram *program, PlumInterner *names, PlumDiagList *diags);

/* One module of a program: its syntax tree, where its errors go, and its
 * file (NULL for the program's own). */
typedef struct PlumCompileModule {
    const PlumProgram *program;
    PlumDiagList *diags;
    const char *path;
} PlumCompileModule;

/* A program of several modules, imports first and the program's own file
 * last; each module's top-level code runs once, in that order. */
PlumCompiled plum_compile_modules(const PlumCompileModule *modules, size_t count,
                                  PlumInterner *names);
void plum_compiled_free(PlumCompiled *compiled);

#endif /* PLUM_COMPILER_COMPILER_H */
