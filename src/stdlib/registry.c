/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "stdlib/native.h"

#include <string.h>

static const PlumNativeModule *const modules[] = {
    &plum_module_math,
    &plum_module_str,
};

#define MODULE_COUNT (sizeof modules / sizeof modules[0])

const PlumNativeModule *plum_native_module(const char *name) {
    for (size_t i = 0; i < MODULE_COUNT; i++) {
        if (strcmp(modules[i]->name, name) == 0) {
            return modules[i];
        }
    }
    return NULL;
}

const PlumNativeModule *plum_native_module_at(size_t i) {
    return i < MODULE_COUNT ? modules[i] : NULL;
}

const PlumNativeFunction *plum_native_function(const PlumNativeModule *m, const char *name) {
    for (size_t i = 0; i < m->function_count; i++) {
        if (strcmp(m->functions[i].name, name) == 0) {
            return &m->functions[i];
        }
    }
    return NULL;
}

const PlumNativeConstant *plum_native_constant(const PlumNativeModule *m, const char *name) {
    for (size_t i = 0; i < m->constant_count; i++) {
        if (strcmp(m->constants[i].name, name) == 0) {
            return &m->constants[i];
        }
    }
    return NULL;
}

/* A function's number: its module's place times 256, plus its own. */
int plum_native_id(const PlumNativeFunction *fn) {
    for (size_t m = 0; m < MODULE_COUNT; m++) {
        const PlumNativeModule *mod = modules[m];
        if (fn >= mod->functions && fn < mod->functions + mod->function_count) {
            return (int)(m * 256 + (size_t)(fn - mod->functions));
        }
    }
    return -1;
}

const PlumNativeFunction *plum_native_by_id(int id) {
    size_t m = (size_t)id / 256, f = (size_t)id % 256;
    if (id < 0 || m >= MODULE_COUNT || f >= modules[m]->function_count) {
        return NULL;
    }
    return &modules[m]->functions[f];
}

/* Signatures are short, so a scan for the `num` type is enough. */
static bool is_num_at(const char *type) {
    return strncmp(type, "num", 3) == 0 &&
           (type[3] == '\0' || type[3] == ',' || type[3] == ')' || type[3] == ' ');
}

bool plum_native_param_is_num(const PlumNativeFunction *fn, size_t i) {
    const char *p = strchr(fn->signature, '(');
    size_t at = 0;
    int depth = 0;
    for (p = p != NULL ? p + 1 : fn->signature; *p != '\0' && *p != ')'; p++) {
        if (*p == '[') {
            depth++;
        } else if (*p == ']') {
            depth--;
        } else if (*p == ',' && depth == 0) {
            at++;
        } else if (*p == ':' && depth == 0 && at == i) {
            p++;
            while (*p == ' ') {
                p++;
            }
            return is_num_at(p);
        }
    }
    return false;
}

bool plum_native_result_is_num(const PlumNativeFunction *fn) {
    const char *arrow = strstr(fn->signature, "->");
    if (arrow == NULL) {
        return false;
    }
    arrow += 2;
    while (*arrow == ' ') {
        arrow++;
    }
    return is_num_at(arrow);
}
