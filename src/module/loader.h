/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Programs of several files (spec section 9). Starting from the program's
 * file, the loader reads, lexes and parses every file it imports with
 * `seed ./path` or `seed package` (src/orchard/packages.h), once each, and orders them so a module
 * comes after the modules it imports. Import cycles and missing files are reported at the `seed`
 * that names them.
 */
#ifndef PLUM_MODULE_LOADER_H
#define PLUM_MODULE_LOADER_H

#include "diag/diag.h"
#include "lexer/lexer.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "util/intern.h"
#include "util/map.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct PlumModule {
    char *path; /* normalized, with '/' */
    char *text; /* owned */
    size_t len;
    PlumSource source;
    PlumDiagList diags;
    PlumTokenList tokens;
    PlumProgram program;
    PlumMap exports; /* name -> PlumSymbol*, after resolution */
    int state;       /* while loading: 0 new, 1 being loaded, 2 done; or PLUM_MODULE_FORGOTTEN */
    int package;     /* 0: the program's project; n: the nth installed package it seeded */
} PlumModule;

/* A module the REPL dropped (its entry failed): kept in memory, since
 * syntax trees may point into it, but a later `seed` loads the file again. */
#define PLUM_MODULE_FORGOTTEN 3

/*
 * Reads an imported file: its text, NUL-terminated and malloc'd, or NULL
 * with *err set to an errno code. The language server uses one to read
 * the files open in the editor from memory instead of from disk.
 */
typedef char *(*PlumModuleReader)(void *ctx, const char *path, size_t *len, int *err);

typedef struct PlumModules {
    PlumModule **items; /* imports first; the program's own file last */
    size_t count;
    PlumModule **all; /* in the order they were found, for freeing */
    size_t all_count;
    PlumArena arena;
    PlumInterner names;
    bool open;                     /* made by plum_modules_init: entries are added one at a time */
    struct PlumPackages *packages; /* the project's packages, once one is seeded */
    PlumModuleReader read;         /* NULL: imported files are read from disk */
    void *read_ctx;
} PlumModules;

/*
 * Load `path` and everything it imports. Returns 0, or an errno code when
 * the program's own file can't be read (then nothing needs freeing).
 * Problems in the files go to each module's diagnostics.
 */
int plum_modules_load(PlumModules *out, const char *path);

/* The same for a program held in memory (the package commands, the fuzz
 * targets): `text` (malloc'd, NUL-terminated at `len`) moves here. `path`
 * names the program in messages, and relative imports are found from it; a
 * path like "<stdlib plum.orchard>" marks a module built into plum. */
void plum_modules_load_text(PlumModules *out, const char *path, char *text, size_t len);

/*
 * The REPL's modules: an empty set, to which each entry is added as a
 * module of its own with plum_modules_add. Free with plum_modules_free.
 */
void plum_modules_init(PlumModules *m);

/*
 * Add `text` (malloc'd; it moves here) as a new module called `path`,
 * loading what it imports that isn't loaded yet. Returns the new module;
 * `*added` receives every module this loaded, imports first and the new
 * one last (an array the caller frees with plum_free), `*count` how many.
 */
PlumModule *plum_modules_add(PlumModules *m, const char *path, char *text, size_t len,
                             PlumModule ***added, size_t *count);

/* Forget modules whose entry failed: a later `seed` loads them afresh. */
void plum_modules_forget(PlumModules *m, PlumModule *const *mods, size_t count);

/* Did any module report an error? */
bool plum_modules_failed(const PlumModules *m);

/* Print every module's diagnostics, imported modules first. */
void plum_modules_print_diags(const PlumModules *m, FILE *out, bool color);

void plum_modules_free(PlumModules *m);

/* The whole file at `path`, NUL-terminated (malloc'd); NULL with *err set. */
char *plum_read_file(const char *path, size_t *len, int *err);

/* `relative` (like "./models/user") from the directory of `from`, with
 * ".plum" added and "." / ".." folded away, into a new string. */
char *plum_module_path(const char *from, const char *relative);

#endif /* PLUM_MODULE_LOADER_H */
