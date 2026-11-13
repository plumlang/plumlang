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

PlumTreeInfo *plum_tree_info_new(const char *name, PlumTreeInfo *parent, int field_count,
                                 const char *const *field_names) {
    PlumTreeInfo *info = plum_alloc_zeroed(1, sizeof *info);
    info->obj.refcount = 1;
    info->obj.type = PLUM_OBJ_TREE_INFO;
    info->name = name;
    info->parent = parent;
    if (parent != NULL) {
        parent->obj.refcount++;
    }
    info->field_count = field_count;
    info->field_names =
        plum_alloc_zeroed(field_count > 0 ? (size_t)field_count : 1, sizeof(const char *));
    for (int i = 0; i < field_count; i++) {
        info->field_names[i] = field_names[i];
    }
    return info;
}

PlumObject *plum_object_new(PlumTreeInfo *tree) {
    size_t n = (size_t)tree->field_count;
    PlumObject *o = plum_alloc(plum_add_size(sizeof *o, plum_mul_size(n, sizeof(PlumValue))));
    o->obj.refcount = 1;
    o->obj.type = PLUM_OBJ_OBJECT;
    o->tree = tree;
    tree->obj.refcount++;
    o->fields = (PlumValue *)(o + 1);
    for (size_t i = 0; i < n; i++) {
        o->fields[i] = plum_none();
    }
    return o;
}

const char *plum_error_name(PlumErrorKind kind) {
    static const char *const names[] = {
#define PLUM_ERROR_NAME(id, name) name,
        PLUM_ERRORS(PLUM_ERROR_NAME)
#undef PLUM_ERROR_NAME
    };
    return (unsigned)kind < PLUM_ERROR_COUNT ? names[kind] : "Error";
}

bool plum_tree_is(const PlumTreeInfo *tree, const PlumTreeInfo *ancestor) {
    for (; tree != NULL; tree = tree->parent) {
        if (tree == ancestor) {
            return true;
        }
    }
    return false;
}

PlumCell *plum_cell_new(PlumValue v) {
    PlumCell *cell = plum_alloc(sizeof *cell);
    cell->obj.refcount = 1;
    cell->obj.type = PLUM_OBJ_CELL;
    cell->value = v;
    return cell;
}

PlumClosure *plum_closure_new(PlumFunction *fn) {
    PlumClosure *c = plum_alloc_zeroed(1, sizeof *c);
    c->obj.refcount = 1;
    c->obj.type = PLUM_OBJ_CLOSURE;
    c->fn = fn;
    fn->obj.refcount++;
    c->count = fn->capture_count;
    c->cells = plum_alloc_zeroed(fn->capture_count > 0 ? (size_t)fn->capture_count : 1,
                                 sizeof(PlumCell *));
    return c;
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

/*
 * Objects freed by one release, waiting for their own references to be let
 * go. Freeing works through this list instead of recursing, so a long chain
 * (a linked list of a million nodes) can't run the C stack out.
 */
typedef struct Pending {
    PlumObj *local[32];
    PlumObj **items;
    size_t len;
    size_t cap;
} Pending;

static void pending_push(Pending *p, PlumObj *obj) {
    if (p->len == p->cap) {
        size_t cap = p->cap * 2;
        if (p->items == p->local) {
            PlumObj **heap = plum_alloc_zeroed(cap, sizeof(PlumObj *));
            memcpy(heap, p->local, p->len * sizeof(PlumObj *));
            p->items = heap;
        } else {
            p->items = plum_realloc_array(p->items, cap, sizeof(PlumObj *));
        }
        p->cap = cap;
    }
    p->items[p->len++] = obj;
}

/* Let go of one reference; an object nothing holds joins the list. */
static void drop_ref(Pending *p, PlumValue v) {
    if (v.type == PLUM_VAL_OBJ && --v.as.obj->refcount == 0) {
        pending_push(p, v.as.obj);
    }
}

static void drop_obj(Pending *p, PlumObj *obj) {
    if (obj != NULL && --obj->refcount == 0) {
        pending_push(p, obj);
    }
}

/* Free one object, handing what it held to the list. */
static void free_object(PlumObj *obj, Pending *p) {
    switch ((PlumObjType)obj->type) {
    case PLUM_OBJ_STRING:
        plum_free(obj);
        return;
    case PLUM_OBJ_FUNCTION: {
        PlumFunction *fn = (PlumFunction *)obj;
        plum_chunk_free(fn->chunk);
        plum_free(fn->chunk);
        plum_free(fn->captures);
        plum_free(fn);
        return;
    }
    case PLUM_OBJ_TREE_INFO: {
        PlumTreeInfo *info = (PlumTreeInfo *)obj;
        drop_obj(p, info->parent != NULL ? &info->parent->obj : NULL);
        plum_free(info->field_names);
        plum_free(info->methods);
        plum_free(info->traits);
        plum_free(info);
        return;
    }
    case PLUM_OBJ_OBJECT: {
        PlumObject *o = (PlumObject *)obj;
        for (int i = 0; i < o->tree->field_count; i++) {
            drop_ref(p, o->fields[i]);
        }
        drop_obj(p, &o->tree->obj);
        plum_free(o);
        return;
    }
    case PLUM_OBJ_CELL: {
        PlumCell *cell = (PlumCell *)obj;
        drop_ref(p, cell->value);
        plum_free(cell);
        return;
    }
    case PLUM_OBJ_CLOSURE: {
        PlumClosure *c = (PlumClosure *)obj;
        for (int i = 0; i < c->count; i++) {
            drop_obj(p, c->cells[i] != NULL ? &c->cells[i]->obj : NULL);
        }
        plum_free(c->cells);
        drop_obj(p, &c->fn->obj);
        plum_free(c);
        return;
    }
    case PLUM_OBJ_LIST: {
        PlumList *list = (PlumList *)obj;
        for (size_t i = 0; i < list->len; i++) {
            drop_ref(p, list->items[i]);
        }
        plum_free(list->items);
        plum_free(list);
        return;
    }
    case PLUM_OBJ_MAP: {
        PlumTable *t = (PlumTable *)obj;
        for (size_t i = 0; i < t->used; i++) {
            drop_ref(p, t->entries[i].key);
            drop_ref(p, t->entries[i].value);
        }
        t->used = 0;
        t->count = 0;
        plum_table_free(t);
        return;
    }
    }
}

void plum_release(PlumValue v) {
    if (v.type != PLUM_VAL_OBJ || --v.as.obj->refcount != 0) {
        return;
    }
    Pending p;
    p.items = p.local;
    p.len = 0;
    p.cap = sizeof p.local / sizeof p.local[0];
    pending_push(&p, v.as.obj);
    while (p.len > 0) {
        free_object(p.items[--p.len], &p);
    }
    if (p.items != p.local) {
        plum_free(p.items);
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
        if (a.as.obj->type == PLUM_OBJ_OBJECT && b.as.obj->type == PLUM_OBJ_OBJECT) {
            /* kind values: the same case with equal contents; tree objects: identity */
            const PlumObject *x = plum_as_object(a), *y = plum_as_object(b);
            if (x->tree->kind == NULL || x->tree != y->tree) {
                return false;
            }
            for (int i = 0; i < x->tree->field_count; i++) {
                if (!plum_values_equal(x->fields[i], y->fields[i])) {
                    return false;
                }
            }
            return true;
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
