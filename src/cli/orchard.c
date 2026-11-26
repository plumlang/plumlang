/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * plum new, add, remove, install, package, publish and search: the package
 * commands (docs/orchard.md). They are a Plumlang program built into plum,
 * src/stdlib/plum/plum_orchard.plum, run here with the command and its
 * arguments as os.args().
 */
#include "cli/orchard.h"

#include "check/flow.h"
#include "check/resolve.h"
#include "check/typecheck.h"
#include "compiler/compiler.h"
#include "module/loader.h"
#include "stdlib/embedded.h"
#include "vm/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const commands[] = {"new",     "add",     "remove", "install",
                                       "package", "publish", "search"};

bool plum_orchard_is_command(const char *name) {
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; i++) {
        if (strcmp(name, commands[i]) == 0) {
            return true;
        }
    }
    return false;
}

int plum_orchard_command(int argc, char **argv) {
    const PlumEmbeddedModule *e = plum_embedded_module("plum.orchard");
    char *text = e != NULL ? malloc(e->len + 1) : NULL;
    if (text == NULL) {
        fputs("plum: error: the package commands aren't built in\n", stderr);
        return 1;
    }
    memcpy(text, e->text, e->len + 1);
    PlumModules modules;
    plum_modules_load_text(&modules, "<stdlib plum.orchard>", text, e->len);
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
    PlumCompileModule *list = ok ? calloc(modules.count, sizeof *list) : NULL;
    for (size_t i = 0; list != NULL && i < modules.count; i++) {
        PlumModule *m = modules.items[i];
        list[i].program = &m->program;
        list[i].diags = &m->diags;
        list[i].path = i + 1 < modules.count ? m->path : NULL;
    }
    if (list != NULL) {
        compiled = plum_compile_modules(list, modules.count, &modules.names);
    }
    free(list);
    int status = 1;
    if (compiled.main != NULL && !plum_modules_failed(&modules)) {
        PlumRunOptions options = {.argc = argc, .argv = argv};
        PlumModule *program = modules.items[modules.count - 1];
        status = plum_vm_run(&compiled, &program->source, &options, stdout, stderr);
    } else {
        /* a bug in plum itself, not in the user's program */
        plum_modules_print_diags(&modules, stderr, plum_diag_use_color(stderr));
    }
    plum_compiled_free(&compiled);
    plum_modules_free(&modules);
    return status;
}
