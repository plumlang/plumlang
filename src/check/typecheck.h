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

#endif /* PLUM_CHECK_TYPECHECK_H */
