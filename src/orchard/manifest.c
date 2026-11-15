/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "orchard/manifest.h"

#include "lexer/lexer.h"
#include "module/loader.h"
#include "stdlib/embedded.h"
#include "stdlib/native.h"
#include "util/alloc.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Versions and ranges                                                 */
/* ------------------------------------------------------------------ */

/* A number without leading zeros, at most 9 digits. Moves *s past it. */
static bool parse_number(const char **s, uint32_t *out) {
    const char *p = *s;
    if (!isdigit((unsigned char)*p) || (p[0] == '0' && isdigit((unsigned char)p[1]))) {
        return false;
    }
    uint32_t v = 0;
    int digits = 0;
    while (isdigit((unsigned char)*p)) {
        if (++digits > 9) {
            return false;
        }
        v = v * 10 + (uint32_t)(*p - '0');
        p++;
    }
    *s = p;
    *out = v;
    return true;
}

/* One to three parts; returns how many, or 0 if it isn't a version. */
static int parse_partial(const char *text, PlumVersion *out) {
    uint32_t parts[3] = {0, 0, 0};
    int n = 0;
    const char *p = text;
    while (n < 3) {
        if (!parse_number(&p, &parts[n])) {
            return 0;
        }
        n++;
        if (*p != '.') {
            break;
        }
        p++;
    }
    if (*p != '\0') {
        return 0;
    }
    out->major = parts[0];
    out->minor = parts[1];
    out->patch = parts[2];
    return n;
}

bool plum_version_parse(const char *text, PlumVersion *out) {
    PlumVersion v;
    if (parse_partial(text, &v) != 3) {
        return false;
    }
    *out = v;
    return true;
}

int plum_version_compare(PlumVersion a, PlumVersion b) {
    if (a.major != b.major) {
        return a.major < b.major ? -1 : 1;
    }
    if (a.minor != b.minor) {
        return a.minor < b.minor ? -1 : 1;
    }
    if (a.patch != b.patch) {
        return a.patch < b.patch ? -1 : 1;
    }
    return 0;
}

static PlumVersion version(uint32_t major, uint32_t minor, uint32_t patch) {
    PlumVersion v = {major, minor, patch};
    return v;
}

bool plum_range_parse(const char *text, PlumRange *out) {
    while (*text == ' ') {
        text++;
    }
    PlumRange r;
    memset(&r, 0, sizeof r);
    if (strcmp(text, "*") == 0) {
        *out = r;
        return true;
    }
    char op = '^';
    if (text[0] == '>' && text[1] == '=') {
        op = '>';
        text += 2;
    } else if (text[0] == '^' || text[0] == '~' || text[0] == '=') {
        op = text[0];
        text++;
    }
    while (*text == ' ') {
        text++;
    }
    PlumVersion v;
    int parts = parse_partial(text, &v);
    if (parts == 0) {
        return false;
    }
    r.low = v;
    r.has_high = op != '>';
    switch (op) {
    case '^': /* the left-most non-zero part stays */
        if (v.major > 0 || parts == 1) {
            r.high = version(v.major + 1, 0, 0);
        } else if (v.minor > 0 || parts == 2) {
            r.high = version(0, v.minor + 1, 0);
        } else {
            r.high = version(0, 0, v.patch + 1);
        }
        break;
    case '~': /* the patch moves; with only a major, the minor too */
        r.high = parts == 1 ? version(v.major + 1, 0, 0) : version(v.major, v.minor + 1, 0);
        break;
    case '=':
        r.high = parts == 3   ? version(v.major, v.minor, v.patch + 1)
                 : parts == 2 ? version(v.major, v.minor + 1, 0)
                              : version(v.major + 1, 0, 0);
        break;
    default:
        break;
    }
    *out = r;
    return true;
}

bool plum_range_allows(const PlumRange *r, PlumVersion v) {
    return plum_version_compare(v, r->low) >= 0 &&
           (!r->has_high || plum_version_compare(v, r->high) < 0);
}

/* ------------------------------------------------------------------ */
/* Names and paths                                                     */
/* ------------------------------------------------------------------ */

/* Is `name` a standard module's first part (net of net.http), or reserved? */
static bool stdlib_name(const char *name) {
    static const char *const reserved[] = {"plum", "std", "stdlib", "orchard"};
    for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
        if (strcmp(name, reserved[i]) == 0) {
            return true;
        }
    }
    size_t n = strlen(name);
    for (size_t i = 0;; i++) {
        const PlumNativeModule *m = plum_native_module_at(i);
        const PlumEmbeddedModule *e = plum_embedded_module_at(i);
        if (m == NULL && e == NULL) {
            return false;
        }
        const char *names[2] = {m != NULL ? m->name : NULL, e != NULL ? e->name : NULL};
        for (int k = 0; k < 2; k++) {
            if (names[k] != NULL && strncmp(names[k], name, n) == 0 &&
                (names[k][n] == '\0' || names[k][n] == '.')) {
                return true;
            }
        }
    }
}

bool plum_package_name_valid(const char *name, char *why, size_t size) {
    size_t n = strlen(name);
    if (n < 2 || n > 40) {
        snprintf(why, size, "a package name has 2 to 40 characters");
        return false;
    }
    if (!(name[0] >= 'a' && name[0] <= 'z')) {
        snprintf(why, size, "a package name starts with a lowercase letter");
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) {
            snprintf(why, size, "a package name has only lowercase letters, digits and '-'");
            return false;
        }
        if (c == '-' && (i + 1 == n || name[i + 1] == '-')) {
            snprintf(why, size, "a '-' in a package name goes between letters or digits");
            return false;
        }
    }
    if (stdlib_name(name)) {
        snprintf(why, size, "'%s' is a standard module's name, which `seed %s` would mean", name,
                 name);
        return false;
    }
    char ident[48];
    plum_package_from_seed(name, ident, sizeof ident); /* the same length */
    for (size_t i = 0; ident[i] != '\0'; i++) {
        if (ident[i] == '-') {
            ident[i] = '_';
        }
    }
    if (plum_is_keyword(ident, strlen(ident))) {
        snprintf(why, size, "'%s' is a keyword, so `seed %s` couldn't name it", ident, ident);
        return false;
    }
    return true;
}

bool plum_package_from_seed(const char *seed_name, char *buf, size_t size) {
    size_t n = strlen(seed_name);
    if (n + 1 > size) {
        return false;
    }
    for (size_t i = 0; i <= n; i++) {
        buf[i] = seed_name[i] == '_' ? '-' : seed_name[i];
    }
    return true;
}

static bool device_name(const char *part, size_t len) {
    /* CON, PRN, AUX, NUL, COM1-9, LPT1-9, with or without an extension */
    size_t stem = 0;
    while (stem < len && part[stem] != '.') {
        stem++;
    }
    char low[8];
    if (stem > 4) {
        return false;
    }
    for (size_t i = 0; i < stem; i++) {
        low[i] = (char)tolower((unsigned char)part[i]);
    }
    low[stem] = '\0';
    static const char *const names[] = {"con", "prn", "aux", "nul"};
    for (size_t i = 0; i < 4; i++) {
        if (strcmp(low, names[i]) == 0) {
            return true;
        }
    }
    return stem == 4 && (strncmp(low, "com", 3) == 0 || strncmp(low, "lpt", 3) == 0) &&
           low[3] >= '1' && low[3] <= '9';
}

const char *plum_package_path_problem(const char *path) {
    size_t n = strlen(path);
    if (n == 0 || n > 200) {
        return "a path in a package has 1 to 200 characters";
    }
    if (path[0] == '/' || path[0] == '\\') {
        return "a path in a package is relative, not absolute";
    }
    size_t parts = 0;
    const char *last = path;
    for (const char *p = path; *p != '\0';) {
        const char *end = strchr(p, '/');
        size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
        if (len == 0) {
            return "a path in a package has no empty parts (\"//\" or a '/' at the end)";
        }
        if ((len == 1 && p[0] == '.') || (len == 2 && p[0] == '.' && p[1] == '.')) {
            return "a path in a package has no \".\" or \"..\" parts";
        }
        if (len > 100) {
            return "each part of a path in a package has at most 100 characters";
        }
        for (size_t i = 0; i < len; i++) {
            char c = p[i];
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '-' || c == '.';
            if (!ok) {
                return c == '\\' || c == ':'
                           ? "a path in a package uses '/', with no '\\', ':' or drive letters"
                           : "a path in a package has only letters, digits, '_', '-', '.' and '/'";
            }
        }
        if (p[0] == '.' || p[0] == '-' || p[len - 1] == '.') {
            return "no part of a path in a package starts with '.' or '-' or ends with '.'";
        }
        if (device_name(p, len)) {
            return "a path in a package can't use a device name like CON or NUL";
        }
        if (++parts > 16) {
            return "a path in a package is at most 16 directories deep";
        }
        last = p;
        p += len + (end != NULL ? 1 : 0);
        if (end != NULL && *p == '\0') {
            return "a path in a package has no empty parts (\"//\" or a '/' at the end)";
        }
    }
    size_t last_len = strlen(last);
    if (last_len > 5 && strcmp(last + last_len - 5, ".plum") == 0) {
        return NULL;
    }
    if (parts == 1 && (strcmp(path, "plum.toml") == 0 || strcmp(path, "README.md") == 0 ||
                       strncmp(path, "LICENSE", 7) == 0)) {
        return NULL;
    }
    return "a package holds .plum files, and plum.toml, README.md and LICENSE* at its top";
}

/* ------------------------------------------------------------------ */
/* Reading TOML files                                                  */
/* ------------------------------------------------------------------ */

static char *join_path(const char *dir, const char *file) {
    size_t d = strlen(dir);
    while (d > 1 && (dir[d - 1] == '/' || dir[d - 1] == '\\')) {
        d--;
    }
    size_t f = strlen(file);
    char *out = plum_alloc(d + f + 2);
    memcpy(out, dir, d);
    out[d] = '/';
    memcpy(out + d + 1, file, f + 1);
    return out;
}

/* Read DIR/FILE and parse it as TOML. 0, -1 (diagnostics), or errno. */
static int read_toml(const char *dir, const char *file, char **path, char **text,
                     PlumSource *source, PlumDiagList *diags, PlumToml *toml) {
    *path = join_path(dir, file);
    size_t len = 0;
    int err = 0;
    *text = plum_read_file(*path, &len, &err);
    source->path = *path;
    source->text = *text != NULL ? *text : "";
    source->length = *text != NULL ? len : 0;
    plum_diag_init(diags, source);
    memset(toml, 0, sizeof *toml);
    plum_arena_init(&toml->arena);
    if (*text == NULL) {
        return err != 0 ? err : EIO;
    }
    plum_arena_free(&toml->arena);
    return plum_toml_parse(toml, source, diags) ? 0 : -1;
}

static PlumSpan start_span(void) {
    PlumSpan s = {0, 0, 1, 1};
    return s;
}

/* A string field of [package]; NULL if absent (or not a string: reported). */
static const char *string_field(PlumManifest *m, const char *key, bool required) {
    const PlumTomlValue *v = plum_toml_get(&m->toml, "package", key);
    if (v == NULL) {
        if (required) {
            const PlumTomlTable *t = plum_toml_table(&m->toml, "package");
            plum_diag_error(&m->diags, t != NULL ? t->span : start_span(), "E0904",
                            "[package] has no %s; add a line like %s = \"%s\"", key, key,
                            strcmp(key, "name") == 0      ? "my-package"
                            : strcmp(key, "version") == 0 ? "0.1.0"
                                                          : "main.plum");
        }
        return NULL;
    }
    if (v->kind != PLUM_TOML_STRING) {
        plum_diag_error(&m->diags, v->value_span, "E0904", "%s is a string, in quotes, not a list",
                        key);
        return NULL;
    }
    return v->str;
}

static const char *const *list_field(PlumManifest *m, const char *key, size_t *count) {
    const PlumTomlValue *v = plum_toml_get(&m->toml, "package", key);
    *count = 0;
    if (v == NULL) {
        return NULL;
    }
    if (v->kind != PLUM_TOML_ARRAY) {
        plum_diag_error(&m->diags, v->value_span, "E0904",
                        "%s is a list of strings, like %s = [\"...\"]", key, key);
        return NULL;
    }
    *count = v->count;
    return v->items;
}

static void check_entry_path(PlumManifest *m, const char *key, const char *value) {
    const PlumTomlValue *v = plum_toml_get(&m->toml, "package", key);
    const char *problem = plum_package_path_problem(value);
    size_t n = strlen(value);
    if (problem == NULL && (n < 6 || strcmp(value + n - 5, ".plum") != 0)) {
        problem = "it names a .plum file";
    }
    if (problem != NULL) {
        plum_diag_error(&m->diags, v->value_span, "E0911", "%s = \"%s\" can't be used: %s", key,
                        value, problem);
    }
}

static bool keyword_ok(const char *k) {
    size_t n = strlen(k);
    if (n == 0 || n > 30) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = k[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

static void check_manifest(PlumManifest *m) {
    static const char *const keys[] = {"name",        "version", "entry",   "lib",
                                       "description", "license", "authors", "repository",
                                       "keywords",    "plum"};
    for (size_t i = 0; i < m->toml.tables.len; i++) {
        const PlumTomlTable *t = &m->toml.tables.items[i];
        if (strcmp(t->name, "package") != 0 && strcmp(t->name, "dependencies") != 0) {
            plum_diag_error(&m->diags, t->span, "E0903",
                            "plum.toml has no table [%s]; the tables are [package] and "
                            "[dependencies]",
                            t->name);
        }
    }
    if (plum_toml_table(&m->toml, "package") == NULL) {
        plum_diag_error(&m->diags, start_span(), "E0904",
                        "plum.toml has no [package] table; start it with [package] and the "
                        "package's name, version and entry");
        return;
    }
    for (size_t i = 0; i < m->toml.values.len; i++) {
        const PlumTomlValue *v = &m->toml.values.items[i];
        if (strcmp(v->table, "package") == 0) {
            bool known = false;
            for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
                known = known || strcmp(v->key, keys[k]) == 0;
            }
            if (!known) {
                plum_diag_error(&m->diags, v->key_span, "E0903",
                                "[package] has no key '%s'; the keys are name, version, entry, "
                                "lib, description, license, authors, repository, keywords and "
                                "plum",
                                v->key);
            }
        } else if (strcmp(v->table, "") == 0) {
            plum_diag_error(&m->diags, v->key_span, "E0903",
                            "'%s' is outside any table; put it under [package]", v->key);
        }
    }
    m->name = string_field(m, "name", true);
    m->version = string_field(m, "version", true);
    m->entry = string_field(m, "entry", true);
    m->lib = string_field(m, "lib", false);
    m->description = string_field(m, "description", false);
    m->license = string_field(m, "license", false);
    m->repository = string_field(m, "repository", false);
    m->plum = string_field(m, "plum", false);
    m->authors = list_field(m, "authors", &m->author_count);
    m->keywords = list_field(m, "keywords", &m->keyword_count);
    char why[160];
    if (m->name != NULL && !plum_package_name_valid(m->name, why, sizeof why)) {
        plum_diag_error(&m->diags, plum_toml_get(&m->toml, "package", "name")->value_span, "E0905",
                        "the name \"%s\" can't be used: %s", m->name, why);
    }
    PlumVersion v;
    if (m->version != NULL && !plum_version_parse(m->version, &v)) {
        plum_diag_error(&m->diags, plum_toml_get(&m->toml, "package", "version")->value_span,
                        "E0906",
                        "\"%s\" isn't a version; write three numbers, MAJOR.MINOR.PATCH, like "
                        "\"1.0.0\"",
                        m->version);
    }
    PlumRange r;
    if (m->plum != NULL && !plum_range_parse(m->plum, &r)) {
        plum_diag_error(&m->diags, plum_toml_get(&m->toml, "package", "plum")->value_span, "E0907",
                        "\"%s\" isn't a version range; write one like \">=0.1\" or \"^1.0\"",
                        m->plum);
    }
    if (m->entry != NULL) {
        check_entry_path(m, "entry", m->entry);
    }
    if (m->lib != NULL) {
        check_entry_path(m, "lib", m->lib);
    } else {
        m->lib = m->entry;
    }
    if (m->description != NULL && strlen(m->description) > 300) {
        plum_diag_error(&m->diags, plum_toml_get(&m->toml, "package", "description")->value_span,
                        "E0904", "the description is longer than 300 bytes; keep it to a line");
    }
    if (m->keyword_count > 10) {
        plum_diag_error(&m->diags, plum_toml_get(&m->toml, "package", "keywords")->value_span,
                        "E0904", "a package has at most 10 keywords");
    }
    for (size_t i = 0; i < m->keyword_count; i++) {
        if (!keyword_ok(m->keywords[i])) {
            plum_diag_error(&m->diags, plum_toml_get(&m->toml, "package", "keywords")->value_span,
                            "E0904",
                            "the keyword \"%s\" can't be used: keywords are 1 to 30 lowercase "
                            "letters, digits and '-'",
                            m->keywords[i]);
        }
    }
    for (size_t i = 0; i < m->toml.values.len; i++) {
        const PlumTomlValue *d = &m->toml.values.items[i];
        if (strcmp(d->table, "dependencies") != 0) {
            continue;
        }
        if (!plum_package_name_valid(d->key, why, sizeof why)) {
            plum_diag_error(&m->diags, d->key_span, "E0905", "the dependency '%s' can't be: %s",
                            d->key, why);
            continue;
        }
        if (m->name != NULL && strcmp(d->key, m->name) == 0) {
            plum_diag_error(&m->diags, d->key_span, "E0905",
                            "a package can't depend on itself ('%s')", d->key);
            continue;
        }
        if (d->kind != PLUM_TOML_STRING) {
            plum_diag_error(&m->diags, d->value_span, "E0904",
                            "a dependency's value is a version range, like %s = \"^1.0\"", d->key);
            continue;
        }
        if (!plum_range_parse(d->str, &r)) {
            plum_diag_error(&m->diags, d->value_span, "E0907",
                            "\"%s\" isn't a version range; use ^1.2, ~1.2.3, =1.2.3, >=1.2 or *",
                            d->str);
            continue;
        }
        PlumDependency dep = {d->key, d->str, d->key_span};
        plum_array_push(&m->deps, dep);
    }
}

int plum_manifest_load(PlumManifest *m, const char *dir) {
    memset(m, 0, sizeof *m);
    int status = read_toml(dir, "plum.toml", &m->path, &m->text, &m->source, &m->diags, &m->toml);
    if (status != 0) {
        return status;
    }
    check_manifest(m);
    return m->diags.errors > 0 ? -1 : 0;
}

void plum_manifest_free(PlumManifest *m) {
    plum_array_free(&m->deps);
    plum_toml_free(&m->toml);
    plum_diag_free(&m->diags);
    free(m->text);
    plum_free(m->path);
    memset(m, 0, sizeof *m);
}

const PlumDependency *plum_manifest_dep(const PlumManifest *m, const char *name) {
    for (size_t i = 0; i < m->deps.len; i++) {
        if (strcmp(m->deps.items[i].name, name) == 0) {
            return &m->deps.items[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* plum.lock                                                           */
/* ------------------------------------------------------------------ */

static bool hex64(const char *s) {
    size_t n = 0;
    for (; s[n] != '\0'; n++) {
        if (!((s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f'))) {
            return false;
        }
    }
    return n == 64;
}

static void check_lock(PlumLock *l) {
    char why[160];
    for (size_t i = 0; i < l->toml.values.len; i++) {
        const PlumTomlValue *v = &l->toml.values.items[i];
        if (strncmp(v->table, "packages.", 9) != 0 || strchr(v->table + 9, '.') != NULL) {
            plum_diag_error(&l->diags, v->key_span, "E0910",
                            "plum.lock holds [packages.NAME] tables only; run `plum install` to "
                            "write it again");
        }
    }
    for (size_t i = 0; i < l->toml.tables.len; i++) {
        const PlumTomlTable *t = &l->toml.tables.items[i];
        if (strcmp(t->name, "packages") == 0) {
            continue;
        }
        if (strncmp(t->name, "packages.", 9) != 0 || strchr(t->name + 9, '.') != NULL) {
            plum_diag_error(&l->diags, t->span, "E0910", "plum.lock has no table [%s]", t->name);
            continue;
        }
        const char *name = t->name + 9;
        if (!plum_package_name_valid(name, why, sizeof why)) {
            plum_diag_error(&l->diags, t->span, "E0910", "'%s' isn't a package name: %s", name,
                            why);
            continue;
        }
        const PlumTomlValue *ver = plum_toml_get(&l->toml, t->name, "version");
        const PlumTomlValue *sha = plum_toml_get(&l->toml, t->name, "sha256");
        const PlumTomlValue *deps = plum_toml_get(&l->toml, t->name, "dependencies");
        PlumVersion parsed;
        if (ver == NULL || ver->kind != PLUM_TOML_STRING ||
            !plum_version_parse(ver->str, &parsed)) {
            plum_diag_error(&l->diags, ver != NULL ? ver->value_span : t->span, "E0910",
                            "[%s] needs version = \"MAJOR.MINOR.PATCH\"", t->name);
            continue;
        }
        if (sha == NULL || sha->kind != PLUM_TOML_STRING || !hex64(sha->str)) {
            plum_diag_error(&l->diags, sha != NULL ? sha->value_span : t->span, "E0910",
                            "[%s] needs sha256 = 64 lowercase hex digits", t->name);
            continue;
        }
        if (deps != NULL && deps->kind != PLUM_TOML_ARRAY) {
            plum_diag_error(&l->diags, deps->value_span, "E0910",
                            "dependencies in plum.lock is a list of names");
            continue;
        }
        PlumLockEntry e = {name, ver->str, sha->str, deps != NULL ? deps->items : NULL,
                           deps != NULL ? deps->count : 0};
        plum_array_push(&l->entries, e);
    }
}

int plum_lock_load(PlumLock *l, const char *dir) {
    memset(l, 0, sizeof *l);
    int status = read_toml(dir, "plum.lock", &l->path, &l->text, &l->source, &l->diags, &l->toml);
    if (status != 0) {
        return status;
    }
    check_lock(l);
    return l->diags.errors > 0 ? -1 : 0;
}

void plum_lock_free(PlumLock *l) {
    plum_array_free(&l->entries);
    plum_toml_free(&l->toml);
    plum_diag_free(&l->diags);
    free(l->text);
    plum_free(l->path);
    memset(l, 0, sizeof *l);
}

const PlumLockEntry *plum_lock_find(const PlumLock *l, const char *name) {
    for (size_t i = 0; i < l->entries.len; i++) {
        if (strcmp(l->entries.items[i].name, name) == 0) {
            return &l->entries.items[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* The cache                                                           */
/* ------------------------------------------------------------------ */

static bool put_path(char *buf, size_t size, const char *base, const char *rest) {
    int n = snprintf(buf, size, "%s%s", base, rest);
    if (n < 0 || (size_t)n >= size) {
        return false;
    }
    for (char *p = buf; *p != '\0'; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
    return true;
}

bool plum_orchard_cache(char *buf, size_t size) {
    const char *home = getenv("PLUM_HOME");
    if (home != NULL && home[0] != '\0') {
        return put_path(buf, size, home, "/orchard");
    }
#if defined(_WIN32)
    const char *local = getenv("LOCALAPPDATA");
    if (local != NULL && local[0] != '\0') {
        return put_path(buf, size, local, "/plum/orchard");
    }
    home = getenv("USERPROFILE");
#else
    home = getenv("HOME");
#endif
    if (home == NULL || home[0] == '\0') {
        return false;
    }
    return put_path(buf, size, home, "/.plum/orchard");
}
