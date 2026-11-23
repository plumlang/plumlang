/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
#include "wasm/playground.h"

#include "check/flow.h"
#include "check/resolve.h"
#include "check/typecheck.h"
#include "compiler/compiler.h"
#include "diag/diag.h"
#include "fmt/format.h"
#include "lsp/analysis.h"
#include "lsp/json.h"
#include "lsp/text.h"
#include "module/loader.h"
#include "util/alloc.h"
#include "vm/vm.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* A malloc'd, NUL-terminated copy: the loader frees what it is given with free(). */
static char *malloc_copy(const char *text, size_t len) {
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        plum_fatal("out of memory");
    }
    if (len > 0) {
        memcpy(copy, text, len);
    }
    copy[len] = '\0';
    return copy;
}

int plum_playground_run(const char *text, size_t len, const PlumPlaygroundLimits *limits, FILE *out,
                        FILE *err) {
    PlumRunOptions options = {0};
    options.sandbox = true;
    options.max_steps = PLUM_PLAYGROUND_STEPS;
    options.max_memory = PLUM_PLAYGROUND_MEMORY;
    options.max_sleep = PLUM_PLAYGROUND_SLEEP;
    if (limits != NULL) {
        if (limits->max_steps > 0) {
            options.max_steps = limits->max_steps;
        }
        if (limits->max_memory > 0) {
            options.max_memory = limits->max_memory;
        }
        if (limits->max_sleep != 0) {
            options.max_sleep = limits->max_sleep > 0 ? limits->max_sleep : 0;
        }
    }
    char name[] = PLUM_PLAYGROUND_PATH;
    char *argv[] = {name, NULL};
    options.argc = 1;
    options.argv = argv;

    PlumModules modules;
    plum_modules_load_text(&modules, PLUM_PLAYGROUND_PATH, malloc_copy(text, len), len);
    /* As `plum run`: each module in order, imports first; one that fails
     * stops the rest. */
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
    if (ok) {
        PlumCompileModule *list = plum_alloc_zeroed(modules.count, sizeof *list);
        for (size_t i = 0; i < modules.count; i++) {
            PlumModule *m = modules.items[i];
            list[i].program = &m->program;
            list[i].diags = &m->diags;
            list[i].path = i + 1 < modules.count ? m->path : NULL;
        }
        compiled = plum_compile_modules(list, modules.count, &modules.names);
        plum_free(list);
    }
    plum_modules_print_diags(&modules, err, false);
    int status = plum_modules_failed(&modules) ? 1 : 0;
    if (compiled.main != NULL && status == 0) {
        fflush(err);
        PlumModule *program = modules.items[modules.count - 1];
        status = plum_vm_run(&compiled, &program->source, &options, out, err);
    }
    fflush(out);
    fflush(err);
    plum_compiled_free(&compiled);
    plum_modules_free(&modules);
    return status;
}

/* ------------------------------------------------------------------ */
/* Diagnostics as JSON                                                 */
/* ------------------------------------------------------------------ */

static int by_offset(const void *a, const void *b) {
    const PlumDiag *x = *(const PlumDiag *const *)a;
    const PlumDiag *y = *(const PlumDiag *const *)b;
    if (x->span.offset != y->span.offset) {
        return x->span.offset < y->span.offset ? -1 : 1;
    }
    return 0;
}

static void write_place(PlumJsonWriter *w, const PlumLspLines *lines, size_t offset,
                        const char *line_key, const char *column_key) {
    long long line, character;
    plum_lsp_position(lines, offset, &line, &character);
    plum_jw_key(w, line_key);
    plum_jw_int(w, line + 1);
    plum_jw_key(w, column_key);
    plum_jw_int(w, character + 1);
}

/* The diagnostics of `list`, about `text`, as a JSON array. */
static void write_diags(PlumJsonWriter *w, const PlumDiagList *list, const char *text, size_t len) {
    size_t n = list->items.len;
    const PlumDiag **sorted = plum_alloc_zeroed(n + 1, sizeof *sorted);
    for (size_t i = 0; i < n; i++) {
        sorted[i] = &list->items.items[i];
    }
    qsort(sorted, n, sizeof *sorted, by_offset);
    PlumLspLines lines;
    plum_lsp_lines_init(&lines, text, len);
    plum_jw_array(w);
    for (size_t i = 0; i < n; i++) {
        const PlumDiag *d = sorted[i];
        size_t start = d->span.offset < len ? d->span.offset : len;
        size_t end = start + d->span.length <= len ? start + d->span.length : len;
        plum_jw_object(w);
        plum_jw_key(w, "severity");
        plum_jw_string(w, d->severity == PLUM_ERROR ? "error" : "warning");
        plum_jw_key(w, "code");
        plum_jw_string(w, d->code);
        plum_jw_key(w, "message");
        plum_jw_string(w, d->message);
        write_place(w, &lines, start, "line", "column");
        write_place(w, &lines, end, "endLine", "endColumn");
        plum_jw_object_end(w);
    }
    plum_jw_array_end(w);
    plum_lsp_lines_free(&lines);
    plum_free(sorted);
}

/* The writer's text, moved into a string of its own. */
static char *take_text(PlumJsonWriter *w) {
    size_t n = 0;
    const char *s = plum_jw_text(w, &n);
    char *copy = plum_alloc(n + 1);
    memcpy(copy, s, n + 1);
    plum_jw_free(w);
    return copy;
}

/* Imports in the playground: there are no other files. */
static char *no_files(void *ctx, const char *path, size_t *len, int *err) {
    (void)ctx;
    (void)path;
    *len = 0;
    *err = ENOENT;
    return NULL;
}

char *plum_playground_check(const char *text, size_t len) {
    PlumLspAnalysis *a = plum_lsp_analyze(PLUM_PLAYGROUND_PATH, text, len, no_files, NULL);
    PlumJsonWriter w;
    plum_jw_init(&w);
    write_diags(&w, &a->main->diags, a->main->text, a->main->len);
    plum_lsp_analysis_free(a);
    return take_text(&w);
}

char *plum_playground_format(const char *text, size_t len) {
    char *copy = malloc_copy(text, len);
    PlumSource source = {PLUM_PLAYGROUND_PATH, copy, len};
    PlumDiagList diags;
    plum_diag_init(&diags, &source);
    size_t out_len = 0;
    char *formatted = plum_format(&source, &diags, &out_len);
    PlumJsonWriter w;
    plum_jw_init(&w);
    plum_jw_object(&w);
    plum_jw_key(&w, "ok");
    plum_jw_bool(&w, formatted != NULL);
    if (formatted != NULL) {
        plum_jw_key(&w, "text");
        plum_jw_stringn(&w, formatted, out_len);
    } else {
        plum_jw_key(&w, "diagnostics");
        write_diags(&w, &diags, copy, len);
    }
    plum_jw_object_end(&w);
    plum_free(formatted);
    plum_diag_free(&diags);
    free(copy);
    return take_text(&w);
}
