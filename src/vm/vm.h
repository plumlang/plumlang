/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The virtual machine: runs compiled bytecode (docs/bytecode.md).
 *
 * A runtime error (integer overflow, division by zero, recursion past the
 * limit, a failed conversion or assertion) ends the program with a message
 * naming the line and the calls that led there, and exit code 1.
 */
#ifndef PLUM_VM_VM_H
#define PLUM_VM_VM_H

#include "compiler/compiler.h"
#include "diag/diag.h"

#include <stdio.h>

/* Calls deeper than this are a runtime error, not a crash (spec 6.5). */
#define PLUM_MAX_FRAMES 10000

/*
 * Run a compiled program. Output from drop goes to `out`, errors to `err`.
 * Returns the exit code: 0, 1 after a runtime error, or the code given to
 * exit().
 */
int plum_vm_run(const PlumCompiled *program, const PlumSource *source, FILE *out, FILE *err);

#endif /* PLUM_VM_VM_H */
