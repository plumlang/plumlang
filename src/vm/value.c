/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "vm/value.h"

#include "compiler/chunk.h"
#include "util/alloc.h"
#include "util/fmt.h"

#include <string.h>

PlumString *plum_string_alloc(size_t len) {
    PlumString *s = plum_alloc(plum_add_size(sizeof(PlumString), plum_add_size(len, 1)));
    s->obj.refcount = 1;
    s->obj.type = PLUM_OBJ_STRING;
    s->len = len;
    s->chars = (char *)(s + 1);
    s->chars[len] = '\0';
    return s;
}

PlumString *plum_string_new(const char *chars, size_t len) {
    PlumString *s = plum_string_alloc(len);
    if (len > 0) {
        memcpy(s->chars, chars, len);
    }
    return s;
}

PlumFunction *plum_function_new(const char *name, int arity) {
    PlumFunction *fn = plum_alloc_zeroed(1, sizeof *fn);
    fn->obj.refcount = 1;
    fn->obj.type = PLUM_OBJ_FUNCTION;
    fn->name = name;
    fn->arity = arity;
    fn->chunk = plum_alloc(sizeof *fn->chunk);
    plum_chunk_init(fn->chunk);
    return fn;
}

static void free_object(PlumObj *obj) {
    switch ((PlumObjType)obj->type) {
    case PLUM_OBJ_STRING:
        plum_free(obj);
        return;
    case PLUM_OBJ_FUNCTION: {
        PlumFunction *fn = (PlumFunction *)obj;
        plum_chunk_free(fn->chunk);
        plum_free(fn->chunk);
        plum_free(fn);
        return;
    }
    }
}

void plum_release(PlumValue v) {
    if (v.type == PLUM_VAL_OBJ && --v.as.obj->refcount == 0) {
        free_object(v.as.obj);
    }
}

bool plum_values_equal(PlumValue a, PlumValue b) {
    if (a.type == PLUM_VAL_INT && b.type == PLUM_VAL_FLOAT) {
        return (double)a.as.i == b.as.f;
    }
    if (a.type == PLUM_VAL_FLOAT && b.type == PLUM_VAL_INT) {
        return a.as.f == (double)b.as.i;
    }
    if (a.type != b.type) {
        return false;
    }
    switch (a.type) {
    case PLUM_VAL_NONE:
        return true;
    case PLUM_VAL_BOOL:
        return a.as.b == b.as.b;
    case PLUM_VAL_INT:
        return a.as.i == b.as.i;
    case PLUM_VAL_FLOAT:
        return a.as.f == b.as.f;
    case PLUM_VAL_OBJ:
        if (a.as.obj == b.as.obj) {
            return true;
        }
        if (a.as.obj->type == PLUM_OBJ_STRING && b.as.obj->type == PLUM_OBJ_STRING) {
            PlumString *x = plum_as_string(a), *y = plum_as_string(b);
            return x->len == y->len && memcmp(x->chars, y->chars, x->len) == 0;
        }
        return false;
    }
    return false;
}

void plum_print_value(FILE *out, PlumValue v) {
    switch (v.type) {
    case PLUM_VAL_NONE:
        fputs("none", out);
        return;
    case PLUM_VAL_BOOL:
        fputs(v.as.b ? "good" : "bad", out);
        return;
    case PLUM_VAL_INT:
        fprintf(out, "%lld", (long long)v.as.i);
        return;
    case PLUM_VAL_FLOAT: {
        char buf[PLUM_FLOAT_BUF];
        fputs(plum_format_float(v.as.f, buf), out);
        return;
    }
    case PLUM_VAL_OBJ:
        switch ((PlumObjType)v.as.obj->type) {
        case PLUM_OBJ_STRING:
            fwrite(plum_as_string(v)->chars, 1, plum_as_string(v)->len, out);
            return;
        case PLUM_OBJ_FUNCTION:
            fprintf(out, "<branch %s>", plum_as_function(v)->name);
            return;
        }
    }
}
