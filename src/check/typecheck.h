/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The type checker (spec sections 3 to 8): gives every expression a type
 * and reports values used where they don't fit. Runs after name
 * resolution, on a program without errors so far.
 *
 * Signatures are known first (parameter and result types are always
 * written), then the top-level statements are checked in order, giving
 * module variables their types, then every function and method body.
 */
#ifndef PLUM_CHECK_TYPECHECK_H
#define PLUM_CHECK_TYPECHECK_H

#include "check/types.h"
#include "diag/diag.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "util/intern.h"

void plum_typecheck(const PlumProgram *program, PlumArena *arena, PlumInterner *names,
                    PlumDiagList *diags);

/* A REPL entry: the same, except that an entry which is one expression
 * may leave its value unused, since the REPL prints it. */
void plum_typecheck_entry(const PlumProgram *program, PlumArena *arena, PlumInterner *names,
                          PlumDiagList *diags);

/* The method an operator calls on a tree's objects (spec 8.5): "plus" for
 * +, "minus" for -, "times" for *, "divided_by" for /, "compare" for
 * < <= > >=; else NULL. */
const char *plum_operator_method(PlumTokenKind op);

#endif /* PLUM_CHECK_TYPECHECK_H */
