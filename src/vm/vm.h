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

/* How deep native code may call back into Plumlang (to_str while printing,
 * map, filter, ...). Each level also nests the C run loop, so this limit
 * keeps the C stack safe; it is far below what the C stack can take, even
 * with Windows' 1 MB stack. */
#define PLUM_MAX_CALLBACK_DEPTH 100

/* How to run a program. */
typedef struct PlumRunOptions {
    int argc; /* the program's own arguments (os.args) */
    char **argv;
    /* Report tree objects still alive in reference cycles when the program
     * ends (spec 11.3), then free them. */
    bool check_leaks;
} PlumRunOptions;

/*
 * Run a compiled program. Output from drop goes to `out`, errors to `err`.
 * Returns the exit code: 0, 1 after a runtime error (or a leak found by
 * the leak check), or the code given to exit().
 */
int plum_vm_run(const PlumCompiled *program, const PlumSource *source,
                const PlumRunOptions *options, FILE *out, FILE *err);

#endif /* PLUM_VM_VM_H */
