/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "vm/value.h"

#include "compiler/chunk.h"
#include "util/alloc.h"
#include "util/fmt.h"
#include "vm/table.h"
#include "vm/text.h"

#include <string.h>

PlumString *plum_string_alloc(size_t len) {
    PlumString *s = plum_alloc(plum_add_size(sizeof(PlumString), plum_add_size(len, 1)));
    s->obj.refcount = 1;
    s->obj.type = PLUM_OBJ_STRING;
    s->len = len;
    s->chars = (char *)(s + 1);
    s->chars[len] = '\0';
    s->char_count = 0;
    s->counted = false;
    return s;
}

size_t plum_string_length(PlumString *s) {
    if (!s->counted) {
        s->char_count = plum_utf8_length(s->chars, s->len);
        s->counted = true;
    }
    return s->char_count;
}

size_t plum_string_offset(PlumString *s, size_t index) {
    if (plum_string_length(s) == s->len) {
        return index; /* ASCII */
    }
    size_t seen = 0, i = 0;
    for (; i < s->len; i++) {
        if (((unsigned char)s->chars[i] & 0xC0) != 0x80) {
            if (seen == index) {
                return i;
            }
            seen++;
        }
    }
    return i;
}

PlumString *plum_string_new(const char *chars, size_t len) {
    PlumString *s = plum_string_alloc(len);
    if (len > 0) {
        memcpy(s->chars, chars, len);
    }
    return s;
}

PlumList *plum_list_new(size_t cap) {
    PlumList *list = plum_alloc_zeroed(1, sizeof *list);
    list->obj.refcount = 1;
    list->obj.type = PLUM_OBJ_LIST;
    if (cap > 0) {
        list->items = plum_alloc_zeroed(cap, sizeof(PlumValue));
        list->cap = cap;
    }
    return list;
}

void plum_list_push(PlumList *list, PlumValue v) {
    if (list->len == list->cap) {
        size_t cap = list->cap < 4 ? 4 : plum_mul_size(list->cap, 2);
        list->items = plum_realloc_array(list->items, cap, sizeof(PlumValue));
        list->cap = cap;
    }
    list->items[list->len++] = v;
}

PlumList *plum_list_unique(PlumValue *slot) {
    PlumList *list = plum_as_list(*slot);
    if (list->obj.refcount == 1) {
        return list;
    }
    PlumList *copy = plum_list_new(list->len);
    for (size_t i = 0; i < list->len; i++) {
        copy->items[i] = plum_retain(list->items[i]);
    }
    copy->len = list->len;
    list->obj.refcount--; /* this holder lets go of the shared one */
    *slot = plum_obj(&copy->obj);
    return copy;
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
    case PLUM_OBJ_LIST: {
        PlumList *list = (PlumList *)obj;
        for (size_t i = 0; i < list->len; i++) {
            plum_release(list->items[i]);
        }
        plum_free(list->items);
        plum_free(list);
        return;
    }
    case PLUM_OBJ_MAP:
        plum_table_free((PlumTable *)obj);
        return;
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
        if (a.as.obj->type == PLUM_OBJ_LIST && b.as.obj->type == PLUM_OBJ_LIST) {
            PlumList *x = plum_as_list(a), *y = plum_as_list(b);
            if (x->len != y->len) {
                return false;
            }
            for (size_t i = 0; i < x->len; i++) {
                if (!plum_values_equal(x->items[i], y->items[i])) {
                    return false;
                }
            }
            return true;
        }
        if (a.as.obj->type == PLUM_OBJ_MAP && b.as.obj->type == PLUM_OBJ_MAP) {
            return plum_tables_equal(plum_as_table(a), plum_as_table(b));
        }
        return false;
    }
    return false;
}

int plum_compare_values(PlumValue a, PlumValue b) {
    if (a.type == PLUM_VAL_OBJ && b.type == PLUM_VAL_OBJ) {
        PlumString *x = plum_as_string(a), *y = plum_as_string(b);
        size_t n = x->len < y->len ? x->len : y->len;
        int c = memcmp(x->chars, y->chars, n);
        return c != 0 ? c : (x->len < y->len ? -1 : x->len > y->len);
    }
    double x = a.type == PLUM_VAL_INT ? (double)a.as.i : a.as.f;
    double y = b.type == PLUM_VAL_INT ? (double)b.as.i : b.as.f;
    if (a.type == PLUM_VAL_INT && b.type == PLUM_VAL_INT) {
        return a.as.i < b.as.i ? -1 : a.as.i > b.as.i;
    }
    return x < y ? -1 : x > y;
}

void plum_print_value(FILE *out, PlumValue v) {
    char error[16];
    PlumString *s = plum_value_text(v, NULL, error, sizeof error);
    fwrite(s->chars, 1, s->len, out);
    plum_release(plum_obj(&s->obj));
}
