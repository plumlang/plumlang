/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The browser playground's core (docs/playground.md): run, check and
 * format one program held in memory. It is plain C, built into every
 * plum and unit-tested natively; src/wasm/bindings.c exports it to
 * JavaScript in the WebAssembly build (make wasm).
 *
 * A run is always sandboxed (no files, network or commands) and limited:
 * a step budget stops an endless loop, a memory budget stops a runaway
 * list, and time.sleep may wait only so long in all. Past a limit the
 * program stops with a message on `err` and exit code 1.
 */
#ifndef PLUM_WASM_PLAYGROUND_H
#define PLUM_WASM_PLAYGROUND_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* What a run's program is called in messages. */
#define PLUM_PLAYGROUND_PATH "main.plum"

/* The default limits: about a second or two of work in a browser. */
#define PLUM_PLAYGROUND_STEPS  50000000ULL
#define PLUM_PLAYGROUND_MEMORY ((int64_t)64 << 20)
#define PLUM_PLAYGROUND_SLEEP  5.0

typedef struct PlumPlaygroundLimits {
    uint64_t max_steps; /* calls and backward jumps; 0: the default */
    int64_t max_memory; /* bytes in use; 0: the default */
    double max_sleep;   /* seconds of time.sleep in all; 0: the default, < 0: none */
} PlumPlaygroundLimits;

/*
 * Check, compile and run `text` (`len` bytes) as `plum run` would, with
 * diagnostics and errors on `err` (no colour) and the program's output on
 * `out`. `limits` may be NULL for the defaults. Returns the exit code: 0,
 * 1 after an error or a limit, or the code given to exit().
 */
int plum_playground_run(const char *text, size_t len, const PlumPlaygroundLimits *limits, FILE *out,
                        FILE *err);

/*
 * Check `text` as `plum check` does. Returns a JSON array of diagnostics
 * (NUL-terminated; free with plum_free), in source order:
 *
 *   [{"severity": "error", "code": "E0301", "message": "...",
 *     "line": 3, "column": 5, "endLine": 3, "endColumn": 9}]
 *
 * Lines and columns count from 1; columns count UTF-16 code units, as
 * JavaScript strings do.
 */
char *plum_playground_check(const char *text, size_t len);

/*
 * Format `text` as `plum fmt` does. Returns JSON (free with plum_free):
 * {"ok": true, "text": "..."}, or {"ok": false, "diagnostics": [...]}
 * when it has syntax errors (as plum_playground_check reports them).
 */
char *plum_playground_format(const char *text, size_t len);

#endif /* PLUM_WASM_PLAYGROUND_H */
