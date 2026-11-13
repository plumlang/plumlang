/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "vm/methods.h"

#include "util/alloc.h"
#include "vm/table.h"
#include "vm/vm.h"

#include <stdio.h>
#include <string.h>

static const struct {
    PlumMethodReceiver receiver;
    const char *name;
    bool mutates;
} methods[] = {
#define PLUM_METHOD_ROW(id, receiver, name, mutates) {PLUM_RECEIVER_##receiver, name, mutates},
    PLUM_METHODS(PLUM_METHOD_ROW)
#undef PLUM_METHOD_ROW
};

int plum_method_find(PlumMethodReceiver receiver, const char *name) {
    for (int i = 0; i < PLUM_METHOD_COUNT; i++) {
        if (methods[i].receiver == receiver && strcmp(methods[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

const char *plum_method_name(PlumMethod m) {
    return (unsigned)m < PLUM_METHOD_COUNT ? methods[m].name : "?";
}

bool plum_method_mutates(PlumMethod m) {
    return (unsigned)m < PLUM_METHOD_COUNT && methods[m].mutates;
}

/* ------------------------------------------------------------------ */
/* Running them                                                        */
/* ------------------------------------------------------------------ */

/* Stable merge sort of values that are all numbers or all strings. */
static void merge_sort(PlumValue *items, PlumValue *tmp, size_t n) {
    if (n < 2) {
        return;
    }
    size_t mid = n / 2;
    merge_sort(items, tmp, mid);
    merge_sort(items + mid, tmp, n - mid);
    size_t i = 0, j = mid, k = 0;
    while (i < mid && j < n) {
        /* <= keeps equal items in their original order */
        tmp[k++] = plum_compare_values(items[i], items[j]) <= 0 ? items[i++] : items[j++];
    }
    while (i < mid) {
        tmp[k++] = items[i++];
    }
    while (j < n) {
        tmp[k++] = items[j++];
    }
    memcpy(items, tmp, n * sizeof(PlumValue));
}

static void sort_list(PlumList *list) {
    if (list->len < 2) {
        return;
    }
    PlumValue *tmp = plum_alloc_zeroed(list->len, sizeof(PlumValue));
    merge_sort(list->items, tmp, list->len);
    plum_free(tmp);
}

static PlumList *copy_list(const PlumList *list) {
    PlumList *copy = plum_list_new(list->len);
    for (size_t i = 0; i < list->len; i++) {
        copy->items[i] = plum_retain(list->items[i]);
    }
    copy->len = list->len;
    return copy;
}

/* Methods of maps and sets. */
static bool run_table_method(PlumMethod m, PlumValue *receiver, const PlumValue *args,
                             PlumValue *result) {
    PlumTable *t = methods[m].mutates ? plum_table_unique(receiver) : plum_as_table(*receiver);
    switch (m) {
    case PLUM_METHOD_MAP_GET: {
        const PlumValue *v = plum_table_find(t, args[0]);
        *result = v != NULL ? plum_retain(*v) : plum_none();
        return true;
    }
    case PLUM_METHOD_MAP_REMOVE:
    case PLUM_METHOD_SET_REMOVE: {
        PlumValue removed;
        bool found = plum_table_remove(t, args[0], &removed);
        if (m == PLUM_METHOD_SET_REMOVE) {
            *result = plum_bool(found);
            if (found) {
                plum_release(removed);
            }
        } else {
            *result = found ? removed : plum_none();
        }
        return true;
    }
    case PLUM_METHOD_MAP_KEYS:
    case PLUM_METHOD_MAP_VALUES: {
        PlumList *out = plum_list_new(t->count);
        int64_t pos = 0;
        const PlumTableEntry *e;
        while ((e = plum_table_next(t, &pos)) != NULL) {
            plum_list_push(out, plum_retain(m == PLUM_METHOD_MAP_KEYS ? e->key : e->value));
        }
        *result = plum_obj(&out->obj);
        return true;
    }
    case PLUM_METHOD_MAP_CLEAR:
    case PLUM_METHOD_SET_CLEAR:
        plum_table_clear(t);
        return true;
    case PLUM_METHOD_SET_ADD:
        plum_table_put(t, plum_retain(args[0]), plum_none());
        return true;
    case PLUM_METHOD_SET_CONTAINS:
        *result = plum_bool(plum_table_find(t, args[0]) != NULL);
        return true;
    default:
        return false;
    }
}

bool plum_run_method(PlumMethod m, PlumValue *receiver, const PlumValue *args, int argc,
                     PlumValue *result, char *error, size_t error_size) {
    *result = plum_none();
    if (methods[m].receiver != PLUM_RECEIVER_LIST) {
        if (run_table_method(m, receiver, args, result)) {
            return true;
        }
        snprintf(error, error_size, "internal error: unknown method %d", (int)m);
        return false;
    }
    PlumList *list = methods[m].mutates ? plum_list_unique(receiver) : plum_as_list(*receiver);

    switch (m) {
    case PLUM_METHOD_LIST_PUSH:
        plum_list_push(list, plum_retain(args[0]));
        return true;
    case PLUM_METHOD_LIST_POP:
        if (list->len == 0) {
            snprintf(error, error_size, "pop() on an empty list");
            return false;
        }
        *result = list->items[--list->len]; /* the reference moves to the result */
        return true;
    case PLUM_METHOD_LIST_INSERT:
    case PLUM_METHOD_LIST_REMOVE_AT: {
        int64_t i = args[0].as.i;
        bool insert = m == PLUM_METHOD_LIST_INSERT;
        int64_t limit = (int64_t)list->len + (insert ? 1 : 0);
        if (i < 0 || i >= limit) {
            snprintf(error, error_size, "%s(%lld) is out of range for a list of %zu item%s",
                     insert ? "insert" : "remove_at", (long long)i, list->len,
                     list->len == 1 ? "" : "s");
            return false;
        }
        size_t at = (size_t)i;
        if (insert) {
            plum_list_push(list, plum_none()); /* make room */
            memmove(&list->items[at + 1], &list->items[at],
                    (list->len - 1 - at) * sizeof(PlumValue));
            list->items[at] = plum_retain(args[1]);
        } else {
            *result = list->items[at];
            memmove(&list->items[at], &list->items[at + 1],
                    (list->len - 1 - at) * sizeof(PlumValue));
            list->len--;
        }
        return true;
    }
    case PLUM_METHOD_LIST_CLEAR:
        for (size_t i = 0; i < list->len; i++) {
            plum_release(list->items[i]);
        }
        list->len = 0;
        return true;
    case PLUM_METHOD_LIST_REVERSE:
        for (size_t i = 0, j = list->len; i + 1 < j; i++, j--) {
            PlumValue t = list->items[i];
            list->items[i] = list->items[j - 1];
            list->items[j - 1] = t;
        }
        return true;
    case PLUM_METHOD_LIST_SORT:
        sort_list(list);
        return true;
    case PLUM_METHOD_LIST_CONTAINS:
    case PLUM_METHOD_LIST_INDEX_OF:
        for (size_t i = 0; i < list->len; i++) {
            if (plum_values_equal(list->items[i], args[0])) {
                *result = m == PLUM_METHOD_LIST_CONTAINS ? plum_bool(true) : plum_int((int64_t)i);
                return true;
            }
        }
        *result = m == PLUM_METHOD_LIST_CONTAINS ? plum_bool(false) : plum_none();
        return true;
    case PLUM_METHOD_LIST_SORTED: {
        PlumList *copy = copy_list(list);
        sort_list(copy);
        *result = plum_obj(&copy->obj);
        return true;
    }
    case PLUM_METHOD_LIST_JOIN: {
        /* the separator defaults to "" (passed as none) */
        const PlumString *sep =
            argc > 0 && args[0].type != PLUM_VAL_NONE ? plum_as_string(args[0]) : NULL;
        size_t sep_len = sep != NULL ? sep->len : 0;
        size_t total = 0;
        for (size_t i = 0; i < list->len; i++) {
            total = plum_add_size(total, plum_as_string(list->items[i])->len);
            if (i > 0) {
                total = plum_add_size(total, sep_len);
            }
        }
        PlumString *s = plum_string_alloc(total);
        size_t at = 0;
        for (size_t i = 0; i < list->len; i++) {
            if (i > 0 && sep_len > 0) {
                memcpy(s->chars + at, sep->chars, sep_len);
                at += sep_len;
            }
            const PlumString *part = plum_as_string(list->items[i]);
            memcpy(s->chars + at, part->chars, part->len);
            at += part->len;
        }
        *result = plum_obj(&s->obj);
        return true;
    }
    default:
        break;
    }
    snprintf(error, error_size, "internal error: unknown method %d", (int)m);
    return false;
}
