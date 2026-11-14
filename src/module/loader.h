/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Programs of several files (spec section 9). Starting from the program's
 * file, the loader reads, lexes and parses every file it imports with
 * `seed ./path`, once each, and orders them so a module comes after the
 * modules it imports. Import cycles and missing files are reported at the
 * `seed` that names them.
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
    int state;       /* while loading: 0 new, 1 being loaded, 2 done */
} PlumModule;

typedef struct PlumModules {
    PlumModule **items; /* imports first; the program's own file last */
    size_t count;
    PlumModule **all; /* in the order they were found, for freeing */
    size_t all_count;
    PlumArena arena;
    PlumInterner names;
} PlumModules;

/*
 * Load `path` and everything it imports. Returns 0, or an errno code when
 * the program's own file can't be read (then nothing needs freeing).
 * Problems in the files go to each module's diagnostics.
 */
int plum_modules_load(PlumModules *out, const char *path);

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
