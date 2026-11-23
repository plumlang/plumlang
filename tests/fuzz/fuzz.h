/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * What the fuzz targets share (see README.md). Each target is a libFuzzer
 * entry point, LLVMFuzzerTestOneInput, built with clang by `make fuzz` on
 * Linux or macOS; none of this is part of the plum binary.
 */
#ifndef PLUM_TESTS_FUZZ_H
#define PLUM_TESTS_FUZZ_H

#define _POSIX_C_SOURCE 200809L /* open_memstream, getpid */

#include "check/flow.h"
#include "check/resolve.h"
#include "check/typecheck.h"
#include "compiler/chunk.h"
#include "compiler/compiler.h"
#include "module/loader.h"
#include "vm/vm.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* How far fuzz_program takes a program. */
typedef enum FuzzStage {
    FUZZ_CHECK,   /* resolve, type check, flow */
    FUZZ_COMPILE, /* ... and compile, and disassemble */
    FUZZ_RUN,     /* ... and run */
} FuzzStage;

/* Where output goes: nowhere, but through all the code that prints it. */
static inline FILE *fuzz_null(void) {
    static FILE *null_file;
    if (null_file == NULL) {
        null_file = fopen("/dev/null", "w");
        if (null_file == NULL) {
            abort();
        }
    }
    return null_file;
}

/* The fuzzer's input, as the malloc'd, NUL-terminated text of a file. */
static inline char *fuzz_text(const uint8_t *data, size_t size) {
    char *text = malloc(size + 1);
    if (text == NULL) {
        abort();
    }
    if (size > 0) {
        memcpy(text, data, size);
    }
    text[size] = '\0';
    return text;
}

/*
 * Take `text` (taken over) as the program fuzz.plum up to `stage`, the way
 * `plum run` does. Returns the exit code.
 */
static inline int fuzz_program(char *text, size_t len, FuzzStage stage,
                               const PlumRunOptions *options) {
    PlumModules modules;
    plum_modules_load_text(&modules, "fuzz.plum", text, len);
    bool ok = !plum_modules_failed(&modules);
    for (size_t i = 0; ok && i < modules.count; i++) {
        PlumModule *m = modules.items[i];
        plum_resolve(&m->program, &modules.arena, &modules.names, &m->diags, &m->exports);
        if (m->diags.errors == 0) {
            plum_typecheck(&m->program, &modules.arena, &modules.names, &m->diags);
        }
        if (m->diags.errors == 0) {
            plum_check_flow(&m->program, &m->diags);
        }
        ok = m->diags.errors == 0;
    }
    PlumCompiled compiled = {0};
    if (ok && stage >= FUZZ_COMPILE) {
        PlumCompileModule *list = calloc(modules.count, sizeof *list);
        if (list == NULL) {
            abort();
        }
        for (size_t i = 0; i < modules.count; i++) {
            PlumModule *m = modules.items[i];
            list[i].program = &m->program;
            list[i].diags = &m->diags;
            list[i].path = i + 1 < modules.count ? m->path : NULL;
        }
        compiled = plum_compile_modules(list, modules.count, &modules.names);
        free(list);
        if (compiled.main != NULL && stage == FUZZ_COMPILE) {
            plum_disassemble(fuzz_null(), compiled.main);
        }
    }
    plum_modules_print_diags(&modules, fuzz_null(), false);
    int status = plum_modules_failed(&modules) ? 1 : 0;
    if (compiled.main != NULL && stage == FUZZ_RUN && status == 0) {
        PlumModule *program = modules.items[modules.count - 1];
        status = plum_vm_run(&compiled, &program->source, options, fuzz_null(), fuzz_null());
    }
    plum_compiled_free(&compiled);
    plum_modules_free(&modules);
    return status;
}

/*
 * The limits of a fuzzed run: enough for a program to do a lot, small
 * enough that hundreds run each second. A sandboxed run can't touch files,
 * the network or other programs.
 */
static inline PlumRunOptions fuzz_run_options(void) {
    static char name[] = "fuzz.plum";
    static char *argv[] = {name, NULL};
    PlumRunOptions o = {0};
    o.argc = 1;
    o.argv = argv;
    o.max_steps = 200000;
    o.max_memory = 256 * 1024 * 1024;
    o.sandbox = true;
    /* Objects left in reference cycles are leaks by design (spec 11.3): the
     * leak check reports them, then frees them. */
    o.check_leaks = true;
    return o;
}

/* Make standard input read `data` (io.read_all, ask()) from a file of this
 * process's own. */
static inline void fuzz_stdin(const uint8_t *data, size_t size) {
    static char path[64];
    if (path[0] == '\0') {
        snprintf(path, sizeof path, "/tmp/plum-fuzz-stdin-%ld", (long)getpid());
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL || (size > 0 && fwrite(data, 1, size, f) != size) || fclose(f) != 0) {
        abort();
    }
    if (freopen(path, "rb", stdin) == NULL) {
        abort();
    }
}

/*
 * Run `driver`, a fixed program that reads the fuzzer's input from
 * standard input and gives it to the code under test. The driver catches
 * the errors that input may rightly cause; any other failure, or output
 * on standard error, is a bug, and the target stops with what it printed.
 */
static inline void fuzz_driver(const char *driver, const uint8_t *data, size_t size, bool sandbox) {
    fuzz_stdin(data, size);
    PlumRunOptions options = fuzz_run_options();
    options.sandbox = sandbox;
    char *err_text = NULL;
    size_t err_len = 0;
    FILE *err = open_memstream(&err_text, &err_len);
    if (err == NULL) {
        abort();
    }
    PlumModules modules;
    size_t len = strlen(driver);
    plum_modules_load_text(&modules, "driver.plum", fuzz_text((const uint8_t *)driver, len), len);
    for (size_t i = 0; i < modules.count; i++) {
        PlumModule *m = modules.items[i];
        plum_resolve(&m->program, &modules.arena, &modules.names, &m->diags, &m->exports);
        plum_typecheck(&m->program, &modules.arena, &modules.names, &m->diags);
        plum_check_flow(&m->program, &m->diags);
    }
    if (plum_modules_failed(&modules)) {
        plum_modules_print_diags(&modules, stderr, false);
        abort(); /* the driver itself is wrong */
    }
    PlumCompileModule list[8];
    if (modules.count > sizeof list / sizeof list[0]) {
        abort();
    }
    for (size_t i = 0; i < modules.count; i++) {
        PlumModule *m = modules.items[i];
        list[i].program = &m->program;
        list[i].diags = &m->diags;
        list[i].path = i + 1 < modules.count ? m->path : NULL;
    }
    PlumCompiled compiled = plum_compile_modules(list, modules.count, &modules.names);
    if (compiled.main == NULL) {
        plum_modules_print_diags(&modules, stderr, false);
        abort();
    }
    PlumModule *program = modules.items[modules.count - 1];
    int status = plum_vm_run(&compiled, &program->source, &options, fuzz_null(), err);
    fclose(err);
    /* the leak check (fuzz_run_options) says so when all is well */
    static const char all_freed[] = "leak check: every object was freed\n";
    bool quiet = err_len == 0 || strcmp(err_text, all_freed) == 0;
    if (status != 0 || !quiet) {
        fprintf(stderr, "driver exited with %d:\n%s\n", status, err_text);
        abort();
    }
    free(err_text);
    plum_compiled_free(&compiled);
    plum_modules_free(&modules);
}

#endif /* PLUM_TESTS_FUZZ_H */
