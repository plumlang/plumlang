/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The compiler turns a checked program into bytecode. It runs only on a
 * program with no errors, so every name is resolved and every expression
 * has a type; it chooses typed instructions from those types and inserts
 * the int-to-float conversion wherever the checker allowed one.
 *
 * Features that arrive in Stage 5 (lists, maps, trees, closures, match,
 * errors) are reported as not supported yet (E0601).
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
} PlumCompiled;

PlumCompiled plum_compile(const PlumProgram *program, PlumInterner *names, PlumDiagList *diags);

#endif /* PLUM_COMPILER_COMPILER_H */
