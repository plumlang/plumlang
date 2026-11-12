/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Control-flow checks, after type checking: functions that don't return a
 * value on every path, break and continue outside loops, match arms that
 * don't cover every case (errors); code that never runs and local
 * variables never used (warnings).
 */
#ifndef PLUM_CHECK_FLOW_H
#define PLUM_CHECK_FLOW_H

#include "diag/diag.h"
#include "parser/ast.h"

void plum_check_flow(const PlumProgram *program, PlumDiagList *diags);

#endif /* PLUM_CHECK_FLOW_H */
