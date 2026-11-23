/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
#include "module/loader.h"

#include "orchard/packages.h"
#include "parser/parser.h"
#include "stdlib/embedded.h"
#include "util/alloc.h"
#include "util/array.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *plum_read_file(const char *path, size_t *len_out, int *err) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        *err = errno;
        return NULL;
    }
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (buf == NULL) {
        fclose(f);
        *err = ENOMEM;
        return NULL;
    }
    for (;;) {
        size_t n = fread(buf + len, 1, cap - len - 1, f);
        len += n;
        if (n == 0) {
            break;
        }
        if (len == cap - 1) {
            if (cap > ((size_t)-1) / 2) {
                free(buf);
                fclose(f);
                *err = EFBIG;
                return NULL;
            }
            char *grown = realloc(buf, cap * 2);
            if (grown == NULL) {
                free(buf);
                fclose(f);
                *err = ENOMEM;
                return NULL;
            }
            buf = grown;
            cap *= 2;
        }
    }
    if (ferror(f)) {
        *err = errno != 0 ? errno : EIO;
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[len] = '\0';
    *len_out = len;
    return buf;
}

/* ------------------------------------------------------------------ */
/* Paths                                                               */
/* ------------------------------------------------------------------ */

static bool is_sep(char c) {
    return c == '/' || c == '\\';
}

/*
 * Fold "." and ".." out of a path, with '/' between parts. A leading ".."
 * that can't fold stays; an absolute path keeps its root.
 */
static char *normalize(const char *path) {
    size_t n = strlen(path);
    char *out = plum_alloc(n + 2);
    size_t len = 0;
    bool absolute = is_sep(path[0]);
    if (absolute) {
        out[len++] = '/';
    }
    size_t base = len; /* parts start here */
    size_t i = 0;
    while (i < n) {
        while (i < n && is_sep(path[i])) {
            i++;
        }
        size_t start = i;
        while (i < n && !is_sep(path[i])) {
            i++;
        }
        size_t part = i - start;
        if (part == 0 || (part == 1 && path[start] == '.')) {
            continue;
        }
        if (part == 2 && path[start] == '.' && path[start + 1] == '.') {
            /* drop the last part, unless there is none (or it is "..") */
            size_t last = len;
            while (last > base && out[last - 1] != '/') {
                last--;
            }
            bool parent_is_dots = len - last == 2 && out[last] == '.' && out[last + 1] == '.';
            if (len > base && !parent_is_dots) {
                len = last > base ? last - 1 : base;
                continue;
            }
            if (absolute) {
                continue; /* above the root is the root */
            }
        }
        if (len > base) {
            out[len++] = '/';
        }
        memcpy(out + len, path + start, part);
        len += part;
    }
    if (len == 0) {
        out[len++] = '.';
    }
    out[len] = '\0';
    return out;
}

char *plum_module_path(const char *from, const char *relative) {
    size_t dir = strlen(from);
    while (dir > 0 && !is_sep(from[dir - 1])) {
        dir--;
    }
    size_t rel = strlen(relative);
    char *joined = plum_alloc(dir + rel + 6);
    memcpy(joined, from, dir);
    memcpy(joined + dir, relative, rel);
    memcpy(joined + dir + rel, ".plum", 6);
    char *out = normalize(joined);
    plum_free(joined);
    return out;
}

/* ------------------------------------------------------------------ */
/* Loading                                                             */
/* ------------------------------------------------------------------ */

typedef struct Loader {
    PlumModules *m;
    PLUM_ARRAY(PlumModule *) order;
    PLUM_ARRAY(PlumModule *) stack; /* the imports being followed, for cycles */
} Loader;

static PlumModule *find(Loader *l, const char *path) {
    for (size_t i = 0; i < l->m->all_count; i++) {
        if (l->m->all[i]->state != PLUM_MODULE_FORGOTTEN && strcmp(l->m->all[i]->path, path) == 0) {
            return l->m->all[i];
        }
    }
    return NULL;
}

static PlumModule *add(Loader *l, char *path, char *text, size_t len) {
    PlumModule *mod = plum_alloc_zeroed(1, sizeof *mod);
    mod->path = path;
    mod->text = text;
    mod->len = len;
    mod->source = (PlumSource){mod->path, text, len};
    plum_diag_init(&mod->diags, &mod->source);
    plum_map_init(&mod->exports);
    l->m->all = plum_realloc_array(l->m->all, l->m->all_count + 1, sizeof(PlumModule *));
    l->m->all[l->m->all_count++] = mod;
    return mod;
}

/* "a.plum -> b.plum -> a.plum" for a cycle ending at `again`. */
static void cycle_text(const Loader *l, const PlumModule *again, char *buf, size_t size) {
    size_t len = 0;
    buf[0] = '\0';
    bool in = false;
    for (size_t i = 0; i < l->stack.len; i++) {
        in = in || l->stack.items[i] == again;
        if (in) {
            int k = snprintf(buf + len, size - len, "%s -> ", l->stack.items[i]->path);
            len = k > 0 && (size_t)k < size - len ? len + (size_t)k : size - 1;
        }
    }
    snprintf(buf + len, size - len, "%s", again->path);
}

static void parse(Loader *l, PlumModule *mod);

/* A standard module written in Plumlang: loaded from the binary, once. */
static PlumModule *import_embedded(Loader *l, const PlumEmbeddedModule *e) {
    char key[96];
    snprintf(key, sizeof key, "<stdlib %s>", e->name);
    PlumModule *mod = find(l, key);
    if (mod != NULL) {
        return mod; /* standard modules don't import their importers: no cycles */
    }
    char *text = malloc(e->len + 1);
    char *path = plum_alloc(strlen(key) + 1);
    if (text == NULL) {
        return NULL;
    }
    memcpy(text, e->text, e->len + 1);
    memcpy(path, key, strlen(key) + 1);
    mod = add(l, path, text, e->len);
    parse(l, mod);
    return mod;
}

/* The module `seed` statement `s` of `importer` names, loading it if new. */
static PlumModule *import(Loader *l, PlumModule *importer, PlumStmt *s) {
    char *path = plum_module_path(importer->path, s->as.seed.path);
    PlumModule *mod = find(l, path);
    if (mod != NULL) {
        plum_free(path);
        if (mod->state == 1) {
            char cycle[1024];
            cycle_text(l, mod, cycle, sizeof cycle);
            plum_diag_error(&importer->diags, s->span, "E0311",
                            "these modules import each other in a circle: %s; move what they "
                            "share into a module of its own",
                            cycle);
            return NULL;
        }
        return mod;
    }
    size_t len = 0;
    int err = 0;
    char *text = l->m->read != NULL ? l->m->read(l->m->read_ctx, path, &len, &err)
                                    : plum_read_file(path, &len, &err);
    if (text == NULL) {
        plum_diag_error(&importer->diags, s->span, "E0308", "can't read the module '%s' (%s): %s",
                        s->as.seed.path, path, strerror(err));
        plum_free(path);
        return NULL;
    }
    mod = add(l, path, text, len);
    mod->package = importer->package; /* a file of the same package */
    parse(l, mod);
    return mod;
}

/* seed weather, seed weather.client: a file of an installed package. */
static PlumModule *import_package(Loader *l, PlumModule *importer, PlumStmt *s) {
    int package = 0;
    char *path = plum_packages_find(&l->m->packages, importer->path, importer->package,
                                    s->as.seed.path, &importer->diags, s->span, &package);
    if (path == NULL) {
        return NULL;
    }
    PlumModule *mod = find(l, path);
    if (mod != NULL) {
        plum_free(path);
        if (mod->state == 1) {
            char cycle[1024];
            cycle_text(l, mod, cycle, sizeof cycle);
            plum_diag_error(&importer->diags, s->span, "E0311",
                            "these modules import each other in a circle: %s; move what they "
                            "share into a module of its own",
                            cycle);
            return NULL;
        }
        return mod;
    }
    size_t len = 0;
    int err = 0;
    char *text = plum_read_file(path, &len, &err);
    if (text == NULL) {
        plum_diag_error(&importer->diags, s->span, "E0308",
                        "can't read the package module '%s' (%s): %s", s->as.seed.path, path,
                        strerror(err));
        plum_free(path);
        return NULL;
    }
    mod = add(l, path, text, len);
    mod->package = package;
    parse(l, mod);
    return mod;
}

static void parse(Loader *l, PlumModule *mod) {
    mod->state = 1;
    plum_array_push(&l->stack, mod);
    plum_lex(&mod->source, &l->m->arena, &l->m->names, &mod->diags, &mod->tokens);
    PlumParser parser;
    plum_parser_init(&parser, &mod->tokens, &l->m->arena, &l->m->names, &mod->diags);
    mod->program = plum_parse_program(&parser);
    for (size_t i = 0; i < mod->program.count; i++) {
        PlumStmt *s = mod->program.stmts[i];
        const PlumEmbeddedModule *embedded =
            s->kind == PLUM_STMT_SEED ? plum_embedded_module(s->as.seed.path) : NULL;
        if (embedded != NULL && strncmp(embedded->name, "plum.", 5) == 0 && mod->path[0] != '<') {
            embedded = NULL; /* plum's own tools, for modules built in only */
        }
        if (embedded != NULL) {
            PlumModule *dep = import_embedded(l, embedded);
            if (dep != NULL) {
                s->as.seed.exports = &dep->exports;
            }
            continue;
        }
        if (s->kind == PLUM_STMT_SEED && s->as.seed.path[0] == '.' &&
            strchr(s->as.seed.path, '<') == NULL) { /* not a path that failed to parse */
            PlumModule *dep = import(l, mod, s);
            if (dep != NULL) {
                s->as.seed.exports = &dep->exports;
            }
        } else if (s->kind == PLUM_STMT_SEED && plum_seed_is_package(s->as.seed.path)) {
            PlumModule *dep = import_package(l, mod, s);
            if (dep != NULL) {
                s->as.seed.exports = &dep->exports;
            }
        }
    }
    l->stack.len--;
    mod->state = 2;
    plum_array_push(&l->order, mod); /* after everything it imports */
}

int plum_modules_load(PlumModules *out, const char *path) {
    memset(out, 0, sizeof *out);
    size_t len = 0;
    int err = 0;
    char *text = plum_read_file(path, &len, &err);
    if (text == NULL) {
        return err;
    }
    plum_modules_load_text(out, path, text, len);
    return 0;
}

void plum_modules_load_text(PlumModules *out, const char *path, char *text, size_t len) {
    memset(out, 0, sizeof *out);
    plum_arena_init(&out->arena);
    plum_interner_init(&out->names);
    Loader l;
    memset(&l, 0, sizeof l);
    l.m = out;
    char *norm = normalize(path);
    PlumModule *main = add(&l, norm, text, len);
    main->source.path = path; /* messages name the program's file as it was given */
    parse(&l, main);
    out->items = l.order.items; /* the array moves to `out` */
    out->count = l.order.len;
    plum_array_free(&l.stack);
}

void plum_modules_init(PlumModules *m) {
    memset(m, 0, sizeof *m);
    plum_arena_init(&m->arena);
    plum_interner_init(&m->names);
    m->open = true;
}

PlumModule *plum_modules_add(PlumModules *m, const char *path, char *text, size_t len,
                             PlumModule ***added, size_t *count) {
    Loader l;
    memset(&l, 0, sizeof l);
    l.m = m;
    char *copy = plum_alloc(strlen(path) + 1);
    memcpy(copy, path, strlen(path) + 1);
    PlumModule *mod = add(&l, copy, text, len);
    parse(&l, mod);
    plum_array_free(&l.stack);
    *added = l.order.items; /* the array moves to the caller */
    *count = l.order.len;
    return mod;
}

void plum_modules_forget(PlumModules *m, PlumModule *const *mods, size_t count) {
    (void)m;
    for (size_t i = 0; i < count; i++) {
        mods[i]->state = PLUM_MODULE_FORGOTTEN;
    }
}

bool plum_modules_failed(const PlumModules *m) {
    for (size_t i = 0; i < m->all_count; i++) {
        if (m->all[i]->diags.errors > 0) {
            return true;
        }
    }
    return false;
}

void plum_modules_print_diags(const PlumModules *m, FILE *out, bool color) {
    for (size_t i = 0; i < m->count; i++) {
        plum_diag_print(&m->items[i]->diags, out, color);
    }
}

void plum_modules_free(PlumModules *m) {
    for (size_t i = 0; i < m->all_count; i++) {
        PlumModule *mod = m->all[i];
        plum_array_free(&mod->tokens);
        plum_diag_free(&mod->diags);
        plum_map_free(&mod->exports);
        plum_free(mod->path);
        free(mod->text);
        plum_free(mod);
    }
    plum_free(m->all);
    plum_free(m->items);
    plum_packages_free(m->packages);
    if (m->all_count > 0 || m->open) {
        plum_interner_free(&m->names);
        plum_arena_free(&m->arena);
    }
    memset(m, 0, sizeof *m);
}
