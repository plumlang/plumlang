/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
#include "cli/pipeline.h"

#include "check/flow.h"
#include "check/resolve.h"
#include "check/typecheck.h"

#include <stdlib.h>

bool plum_cli_check(PlumModules *modules) {
    bool ok = !plum_modules_failed(modules);
    for (size_t i = 0; ok && i < modules->count; i++) {
        PlumModule *m = modules->items[i];
        plum_resolve(&m->program, &modules->arena, &modules->names, &m->diags, &m->exports);
        if (m->diags.errors == 0) {
            plum_typecheck(&m->program, &modules->arena, &modules->names, &m->diags);
        }
        if (m->diags.errors == 0) {
            plum_check_flow(&m->program, &m->diags);
        }
        ok = m->diags.errors == 0;
    }
    return ok;
}

PlumCompiled plum_cli_compile(PlumModules *modules) {
    PlumCompiled compiled = {0};
    PlumCompileModule *list = calloc(modules->count, sizeof *list);
    if (list == NULL) {
        return compiled;
    }
    for (size_t i = 0; i < modules->count; i++) {
        PlumModule *m = modules->items[i];
        list[i].program = &m->program;
        list[i].diags = &m->diags;
        list[i].path = i + 1 < modules->count ? m->path : NULL;
    }
    compiled = plum_compile_modules(list, modules->count, &modules->names);
    free(list);
    return compiled;
}
