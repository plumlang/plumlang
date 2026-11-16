/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * plum.pkg: what the package commands (src/stdlib/plum/plum_orchard.plum)
 * need from C. Reading and changing plum.toml and plum.lock, versions and
 * ranges, the rules for names and paths, the package cache, and `plum
 * check` of a file. An internal module: only modules built into plum can
 * seed it (plum_native_is_internal).
 */
#include "stdlib/native.h"
#include "check/flow.h"
#include "check/resolve.h"
#include "check/typecheck.h"
#include "module/loader.h"
#include "orchard/manifest.h"
#include "plum/version.h"
#include "vm/table.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARGS PlumNative *n, const PlumValue *args, int argc, PlumValue *result

/* A str argument as a C string, refusing zero bytes. */
static const char *text_of(PlumNative *n, const char *what, PlumValue v) {
    const PlumString *s = plum_as_string(v);
    if (memchr(s->chars, '\0', s->len) != NULL) {
        plum_native_throw(n, PLUM_ERROR_VALUE, "%s(): the text contains a zero byte", what);
        return NULL;
    }
    return s->chars;
}

static PlumValue str_or_none(const char *s) {
    return s != NULL ? plum_str_value(s, strlen(s)) : plum_none();
}

static void put(PlumTable *t, const char *key, PlumValue value) {
    plum_table_put(t, plum_str_value(key, strlen(key)), value);
}

static PlumValue string_list(const char *const *items, size_t count) {
    PlumList *list = plum_list_new(count);
    for (size_t i = 0; i < count; i++) {
        plum_list_push(list, plum_str_value(items[i], strlen(items[i])));
    }
    return plum_obj(&list->obj);
}

/* Print a file's problems where the user sees them, then throw. */
static bool report(PlumNative *n, const PlumDiagList *diags, const char *what) {
    FILE *err = plum_native_err(n);
    fflush(plum_native_out(n));
    plum_diag_print(diags, err, plum_diag_use_color(err));
    return plum_native_throw(n, PLUM_ERROR_PARSE, "%s has problems (above)", what);
}

/* manifest(dir): plum.toml as a Json object. */
static bool pkg_manifest(ARGS) {
    (void)argc;
    const char *dir = text_of(n, "manifest", args[0]);
    if (dir == NULL) {
        return false;
    }
    PlumManifest m;
    int status = plum_manifest_load(&m, dir);
    if (status > 0) {
        plum_native_throw(n, PLUM_ERROR_IO, "there is no plum.toml in %s (%s)", dir,
                          strerror(status));
        plum_manifest_free(&m);
        return false;
    }
    if (status < 0) {
        report(n, &m.diags, m.path);
        plum_manifest_free(&m);
        return false;
    }
    PlumTable *t = plum_native_new_map(n);
    put(t, "name", str_or_none(m.name));
    put(t, "version", str_or_none(m.version));
    put(t, "entry", str_or_none(m.entry));
    put(t, "lib", str_or_none(m.lib));
    put(t, "description", str_or_none(m.description));
    put(t, "license", str_or_none(m.license));
    put(t, "repository", str_or_none(m.repository));
    put(t, "plum", str_or_none(m.plum));
    put(t, "authors", string_list(m.authors, m.author_count));
    put(t, "keywords", string_list(m.keywords, m.keyword_count));
    PlumTable *deps = plum_native_new_map(n);
    for (size_t i = 0; i < m.deps.len; i++) {
        put(deps, m.deps.items[i].name, str_or_none(m.deps.items[i].range));
    }
    put(t, "dependencies", plum_obj(&deps->obj));
    plum_manifest_free(&m);
    *result = plum_obj(&t->obj);
    return true;
}

/* lock(dir): plum.lock as {name: {version, sha256, dependencies}}, or none. */
static bool pkg_lock(ARGS) {
    (void)argc;
    const char *dir = text_of(n, "lock", args[0]);
    if (dir == NULL) {
        return false;
    }
    PlumLock l;
    int status = plum_lock_load(&l, dir);
    if (status > 0) {
        plum_lock_free(&l);
        *result = plum_none();
        return true;
    }
    if (status < 0) {
        report(n, &l.diags, l.path);
        plum_lock_free(&l);
        return false;
    }
    PlumTable *t = plum_native_new_map(n);
    for (size_t i = 0; i < l.entries.len; i++) {
        const PlumLockEntry *e = &l.entries.items[i];
        PlumTable *entry = plum_native_new_map(n);
        put(entry, "version", str_or_none(e->version));
        put(entry, "sha256", str_or_none(e->sha256));
        put(entry, "dependencies", string_list(e->deps, e->dep_count));
        put(t, e->name, plum_obj(&entry->obj));
    }
    plum_lock_free(&l);
    *result = plum_obj(&t->obj);
    return true;
}

typedef PLUM_ARRAY(char) Buf;

static void add_text(Buf *b, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        plum_array_push(b, s[i]);
    }
}

/* The byte where line `line` (1-based) starts, or the end. */
static size_t line_start(const char *text, size_t len, uint32_t line) {
    size_t at = 0;
    for (uint32_t l = 1; l < line && at < len; at++) {
        if (text[at] == '\n') {
            l++;
        }
    }
    return at;
}

/*
 * set_dependency(dir, name, range): set or (with none) remove `name` in
 * [dependencies], leaving the rest of plum.toml as it is.
 */
static bool pkg_set_dependency(ARGS) {
    (void)argc;
    const char *dir = text_of(n, "set_dependency", args[0]);
    const char *name = dir != NULL ? text_of(n, "set_dependency", args[1]) : NULL;
    if (name == NULL) {
        return false;
    }
    char why[160];
    if (!plum_package_name_valid(name, why, sizeof why)) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "'%s' isn't a package name: %s", name, why);
    }
    const char *range = NULL;
    if (args[2].type != PLUM_VAL_NONE) {
        range = text_of(n, "set_dependency", args[2]);
        PlumRange r;
        if (range == NULL || !plum_range_parse(range, &r)) {
            return range == NULL ? false
                                 : plum_native_throw(n, PLUM_ERROR_VALUE,
                                                     "'%s' isn't a version range", range);
        }
    }
    PlumManifest m;
    int status = plum_manifest_load(&m, dir);
    if (status != 0) {
        bool ok = status > 0
                      ? plum_native_throw(n, PLUM_ERROR_IO, "there is no plum.toml in %s", dir)
                      : report(n, &m.diags, m.path);
        plum_manifest_free(&m);
        return ok;
    }
    const char *text = m.text;
    size_t len = m.source.length;
    Buf out = {0};
    char line[160];
    if (range != NULL) {
        snprintf(line, sizeof line, "%s = \"%s\"\n", name, range);
    }
    const PlumTomlValue *old = plum_toml_get(&m.toml, "dependencies", name);
    const PlumTomlTable *table = plum_toml_table(&m.toml, "dependencies");
    if (old != NULL) {
        /* replace (or drop) the lines the old value took */
        size_t from = line_start(text, len, old->key_span.line);
        size_t to = line_start(text, len, old->last_line + 1);
        add_text(&out, text, from);
        if (range != NULL) {
            add_text(&out, line, strlen(line));
        }
        add_text(&out, text + to, len - to);
    } else if (range == NULL) {
        plum_manifest_free(&m);
        return plum_native_throw(n, PLUM_ERROR_VALUE, "'%s' isn't in [dependencies]", name);
    } else if (table != NULL) {
        size_t at = line_start(text, len, table->last_line + 1);
        add_text(&out, text, at);
        if (at > 0 && text[at - 1] != '\n') {
            plum_array_push(&out, '\n');
        }
        add_text(&out, line, strlen(line));
        add_text(&out, text + at, len - at);
    } else {
        add_text(&out, text, len);
        if (len > 0 && text[len - 1] != '\n') {
            plum_array_push(&out, '\n');
        }
        add_text(&out, "\n[dependencies]\n", 16);
        add_text(&out, line, strlen(line));
    }
    FILE *f = fopen(m.path, "wb");
    bool ok = f != NULL && fwrite(out.items, 1, out.len, f) == out.len;
    if (f != NULL && fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        plum_native_throw(n, PLUM_ERROR_IO, "can't write %s: %s", m.path, strerror(errno));
    }
    plum_array_free(&out);
    plum_manifest_free(&m);
    *result = plum_none();
    return ok;
}

static bool pkg_satisfies(ARGS) {
    (void)argc;
    PlumVersion v;
    PlumRange r;
    const char *version = text_of(n, "satisfies", args[0]);
    const char *range = version != NULL ? text_of(n, "satisfies", args[1]) : NULL;
    if (range == NULL) {
        return false;
    }
    if (!plum_version_parse(version, &v)) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "'%s' isn't a version", version);
    }
    if (!plum_range_parse(range, &r)) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "'%s' isn't a version range", range);
    }
    *result = plum_bool(plum_range_allows(&r, v));
    return true;
}

static bool pkg_compare(ARGS) {
    (void)argc;
    PlumVersion a, b;
    const char *x = text_of(n, "compare", args[0]);
    const char *y = x != NULL ? text_of(n, "compare", args[1]) : NULL;
    if (y == NULL) {
        return false;
    }
    if (!plum_version_parse(x, &a) || !plum_version_parse(y, &b)) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "'%s' or '%s' isn't a version", x, y);
    }
    *result = plum_int(plum_version_compare(a, b));
    return true;
}

static bool pkg_is_version(ARGS) {
    (void)argc;
    (void)n;
    const PlumString *s = plum_as_string(args[0]);
    PlumVersion v;
    *result = plum_bool(memchr(s->chars, '\0', s->len) == NULL && plum_version_parse(s->chars, &v));
    return true;
}

static bool pkg_is_range(ARGS) {
    (void)argc;
    (void)n;
    const PlumString *s = plum_as_string(args[0]);
    PlumRange r;
    *result = plum_bool(memchr(s->chars, '\0', s->len) == NULL && plum_range_parse(s->chars, &r));
    return true;
}

static bool pkg_name_problem(ARGS) {
    (void)argc;
    (void)n;
    const PlumString *s = plum_as_string(args[0]);
    char why[160];
    if (memchr(s->chars, '\0', s->len) != NULL) {
        snprintf(why, sizeof why, "a package name has no zero bytes");
    } else if (plum_package_name_valid(s->chars, why, sizeof why)) {
        *result = plum_none();
        return true;
    }
    *result = plum_str_value(why, strlen(why));
    return true;
}

static bool pkg_path_problem(ARGS) {
    (void)argc;
    (void)n;
    const PlumString *s = plum_as_string(args[0]);
    const char *why = memchr(s->chars, '\0', s->len) != NULL ? "a path has no zero bytes"
                                                             : plum_package_path_problem(s->chars);
    *result = str_or_none(why);
    return true;
}

static bool pkg_is_text(ARGS) {
    (void)argc;
    (void)n;
    const PlumString *s = plum_as_string(args[0]);
    *result = plum_bool(plum_utf8_valid(s->chars, s->len));
    return true;
}

static bool pkg_cache(ARGS) {
    (void)argc;
    (void)args;
    char buf[4096];
    if (!plum_orchard_cache(buf, sizeof buf)) {
        return plum_native_throw(n, PLUM_ERROR_IO,
                                 "can't find the package cache: set PLUM_HOME, or HOME "
                                 "(USERPROFILE on Windows)");
    }
    *result = plum_str_value(buf, strlen(buf));
    return true;
}

static bool pkg_plum_version(ARGS) {
    (void)argc;
    (void)args;
    (void)n;
    *result = plum_str_value(PLUM_VERSION, strlen(PLUM_VERSION));
    return true;
}

/* check(path): `plum check` of one file; its errors go to standard error. */
static bool pkg_check(ARGS) {
    (void)argc;
    const char *path = text_of(n, "check", args[0]);
    if (path == NULL) {
        return false;
    }
    PlumModules modules;
    int err = plum_modules_load(&modules, path);
    if (err != 0) {
        return plum_native_throw(n, PLUM_ERROR_IO, "can't read %s: %s", path, strerror(err));
    }
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
    size_t errors = 0;
    for (size_t i = 0; i < modules.all_count; i++) {
        errors += modules.all[i]->diags.errors;
    }
    FILE *out = plum_native_err(n);
    fflush(plum_native_out(n));
    plum_modules_print_diags(&modules, out, plum_diag_use_color(out));
    plum_modules_free(&modules);
    *result = plum_int((int64_t)errors);
    return true;
}

static const PlumNativeFunction functions[] = {
    {"manifest", "(dir: str) -> Json", pkg_manifest, "DIR/plum.toml, checked, as an object"},
    {"lock", "(dir: str) -> Json", pkg_lock, "DIR/plum.lock as {name: {version, sha256, ...}}"},
    {"set_dependency", "(dir: str, name: str, range: str?)", pkg_set_dependency,
     "set or remove a dependency in DIR/plum.toml"},
    {"satisfies", "(version: str, range: str) -> bool", pkg_satisfies,
     "is the version in the range?"},
    {"compare", "(a: str, b: str) -> int", pkg_compare, "-1, 0 or 1"},
    {"is_version", "(text: str) -> bool", pkg_is_version, "MAJOR.MINOR.PATCH?"},
    {"is_range", "(text: str) -> bool", pkg_is_range, "a version range?"},
    {"name_problem", "(name: str) -> str?", pkg_name_problem, "why a name can't be a package's"},
    {"path_problem", "(path: str) -> str?", pkg_path_problem, "why a package can't hold a path"},
    {"is_text", "(text: str) -> bool", pkg_is_text, "UTF-8 without zero bytes?"},
    {"cache", "() -> str", pkg_cache, "the package cache directory"},
    {"plum_version", "() -> str", pkg_plum_version, "this plum's version"},
    {"check", "(path: str) -> int", pkg_check, "plum check of a file: its error count"},
};

const PlumNativeModule plum_module_pkg = {
    "plum.pkg", "the package commands' helpers (internal)",
    functions,  sizeof functions / sizeof functions[0],
    NULL,       0,
};
