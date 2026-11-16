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

#include <stdint.h>
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
    /* Print to `err`, when the program ends, how much memory it used: the
     * peak, the allocations, and the values it made by kind (spec 11.5). */
    bool mem_stats;
    /* Limits for running code no one has read (the fuzz targets,
     * tests/fuzz); not reachable from the command line. 0 means none. Past
     * either one the program stops, as if by exit(1), with a message. */
    uint64_t max_steps; /* calls and backward jumps: bounds the run's time */
    int64_t max_memory; /* bytes in use, checked at each step */
    /* Native functions that reach outside the process (files, the network,
     * running commands, sleeping) throw an IOError instead. */
    bool sandbox;
} PlumRunOptions;

/*
 * Run a compiled program. Output from drop goes to `out`, errors to `err`.
 * Returns the exit code: 0, 1 after a runtime error (or a leak found by
 * the leak check), or the code given to exit().
 */
int plum_vm_run(const PlumCompiled *program, const PlumSource *source,
                const PlumRunOptions *options, FILE *out, FILE *err);

/*
 * The REPL: one VM for a whole session, running one entry after another.
 * The globals stay from entry to entry (new ones start as none). Each
 * entry runs until its main task ends; tasks it spawned that are still
 * running carry on while later entries run. `options` must outlive the VM.
 */
typedef struct PlumVM PlumVM;

PlumVM *plum_vm_new(const PlumRunOptions *options, FILE *out, FILE *err);

/*
 * Run an entry: program->main, with the globals and error trees `program`
 * describes. Errors are reported like plum_vm_run's. Returns 0, 1 after
 * an error, or the code given to exit() (then plum_vm_exited is true).
 */
int plum_vm_run_entry(PlumVM *vm, const PlumCompiled *program, const PlumSource *source);

/* Has a program called exit()? */
bool plum_vm_exited(const PlumVM *vm);

void plum_vm_free(PlumVM *vm);

#endif /* PLUM_VM_VM_H */
