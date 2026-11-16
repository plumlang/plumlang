/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "orchard/packages.h"

#include "orchard/manifest.h"
#include "stdlib/embedded.h"
#include "stdlib/native.h"
#include "stdlib/platform.h"
#include "util/alloc.h"
#include "util/array.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define PATH_SIZE 4096

typedef struct Installed {
    char *name;
    char *dir; /* CACHE/NAME/VERSION */
    PlumManifest manifest;
    bool ok; /* its manifest is good and matches the lock */
} Installed;

struct PlumPackages {
    char *root;      /* the project's directory, or NULL when there is none */
    int root_status; /* plum_manifest_load's result */
    PlumManifest manifest;
    int lock_status; /* plum_lock_load's result */
    PlumLock lock;
    PLUM_ARRAY(Installed *) installed; /* package n is installed.items[n - 1] */
};

bool plum_seed_is_package(const char *path) {
    if (path[0] == '.' || path[0] == '\0' || strchr(path, '<') != NULL) {
        return false;
    }
    size_t first = strcspn(path, ".");
    for (size_t i = 0;; i++) {
        const PlumNativeModule *m = plum_native_module_at(i);
        const PlumEmbeddedModule *e = plum_embedded_module_at(i);
        if (m == NULL && e == NULL) {
            break;
        }
        const char *names[2] = {m != NULL ? m->name : NULL, e != NULL ? e->name : NULL};
        for (int k = 0; k < 2; k++) {
            if (names[k] != NULL && strncmp(names[k], path, first) == 0 &&
                (names[k][first] == '\0' || names[k][first] == '.')) {
                return false;
            }
        }
    }
    /* internal modules (plum.*) aren't listed, but are standard too */
    return !(first == 4 && strncmp(path, "plum", 4) == 0);
}

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        fclose(f);
    }
    return f != NULL;
}

static bool is_absolute(const char *path) {
    return path[0] == '/' || path[0] == '\\' ||
           (path[0] != '\0' && path[1] == ':' && (path[2] == '/' || path[2] == '\\'));
}

/* The nearest directory with a plum.toml, from the directory of `from`
 * upwards; NULL if there is none. */
static char *find_root(const char *from) {
    char dir[PATH_SIZE];
    char cwd[PATH_SIZE];
    if (!plum_os_cwd(cwd, sizeof cwd)) {
        return NULL;
    }
    int n;
    if (from[0] == '<') { /* the REPL, or a module built in: the working directory */
        n = snprintf(dir, sizeof dir, "%s", cwd);
    } else if (is_absolute(from)) {
        n = snprintf(dir, sizeof dir, "%s", from);
    } else {
        n = snprintf(dir, sizeof dir, "%s/%s", cwd, from);
    }
    if (n < 0 || (size_t)n >= sizeof dir) {
        return NULL;
    }
    for (char *c = dir; *c != '\0'; c++) {
        if (*c == '\\') {
            *c = '/';
        }
    }
    if (from[0] != '<') { /* the file's own directory */
        char *slash = strrchr(dir, '/');
        if (slash == NULL) {
            return NULL;
        }
        *slash = '\0';
    }
    for (;;) {
        size_t len = strlen(dir);
        if (len + 11 >= sizeof dir) {
            return NULL;
        }
        memcpy(dir + len, "/plum.toml", 11);
        bool found = file_exists(dir);
        dir[len] = '\0';
        if (found) {
            return plum_strdup(len > 0 ? dir : "/");
        }
        char *slash = strrchr(dir, '/');
        if (slash == NULL || len == 0) {
            return NULL;
        }
        *slash = '\0';
    }
}

void plum_packages_free(PlumPackages *p) {
    if (p == NULL) {
        return;
    }
    for (size_t i = 0; i < p->installed.len; i++) {
        Installed *in = p->installed.items[i];
        plum_manifest_free(&in->manifest);
        plum_free(in->name);
        plum_free(in->dir);
        plum_free(in);
    }
    plum_array_free(&p->installed);
    if (p->root != NULL) {
        plum_manifest_free(&p->manifest);
        plum_lock_free(&p->lock);
        plum_free(p->root);
    }
    plum_free(p);
}

/* The first problem in a manifest or lock file, as one line. */
static void first_problem(const PlumDiagList *d, char *buf, size_t size) {
    if (d->items.len == 0) {
        snprintf(buf, size, "it can't be read");
        return;
    }
    const PlumDiag *x = &d->items.items[0];
    snprintf(buf, size, "%s:%u:%u: %s", d->source->path, (unsigned)x->span.line,
             (unsigned)x->span.column, x->message);
}

char *plum_packages_find(PlumPackages **pp, const char *from_path, int from_package,
                         const char *path, PlumDiagList *diags, PlumSpan span, int *package) {
    *package = -1;
    if (*pp == NULL) {
        PlumPackages *p = plum_alloc_zeroed(1, sizeof *p);
        p->root = find_root(from_path);
        if (p->root != NULL) {
            p->root_status = plum_manifest_load(&p->manifest, p->root);
            p->lock_status = plum_lock_load(&p->lock, p->root);
        }
        *pp = p;
    }
    PlumPackages *p = *pp;
    if (p->root == NULL) {
        return NULL; /* no project: a standard module that doesn't exist */
    }

    char name[64];
    char problem[600];
    size_t first = strcspn(path, ".");
    if (first >= sizeof name) {
        first = sizeof name - 1;
    }
    memcpy(name, path, first);
    name[first] = '\0';
    plum_package_from_seed(name, name, sizeof name);

    /* Who is asking: the project, or an installed package. */
    const PlumManifest *asking = NULL;
    const char *asking_name = NULL;
    if (from_package > 0 && (size_t)from_package <= p->installed.len) {
        Installed *in = p->installed.items[from_package - 1];
        asking = &in->manifest;
        asking_name = in->name;
    } else {
        if (p->root_status != 0) {
            first_problem(&p->manifest.diags, problem, sizeof problem);
            plum_diag_error(
                diags, span, p->root_status > 0 ? "E0909" : "E0901",
                "can't seed the package '%s': the project's plum.toml has a problem: %s", name,
                problem);
            return NULL;
        }
        asking = &p->manifest;
    }
    if (plum_manifest_dep(asking, name) == NULL) {
        if (asking_name != NULL) {
            plum_diag_error(diags, span, "E0908",
                            "the package '%s' seeds '%s', which isn't in its [dependencies]",
                            asking_name, name);
        } else {
            plum_diag_error(diags, span, "E0908",
                            "'%s' isn't a standard module or a dependency in plum.toml; to use "
                            "the package, run `plum add %s`",
                            path, name);
        }
        return NULL;
    }
    if (p->lock_status != 0) {
        if (p->lock_status > 0) {
            plum_diag_error(diags, span, "E0909",
                            "the package '%s' isn't installed (the project has no plum.lock); "
                            "run `plum install`",
                            name);
        } else {
            first_problem(&p->lock.diags, problem, sizeof problem);
            plum_diag_error(diags, span, "E0910", "plum.lock has a problem: %s", problem);
        }
        return NULL;
    }
    const PlumLockEntry *locked = plum_lock_find(&p->lock, name);
    if (locked == NULL) {
        plum_diag_error(diags, span, "E0909",
                        "the package '%s' isn't in plum.lock; run `plum install`", name);
        return NULL;
    }

    /* The installed package, read once. */
    Installed *in = NULL;
    int index = 0;
    for (size_t i = 0; i < p->installed.len; i++) {
        if (strcmp(p->installed.items[i]->name, name) == 0) {
            in = p->installed.items[i];
            index = (int)i + 1;
        }
    }
    if (in == NULL) {
        char cache[PATH_SIZE];
        if (!plum_orchard_cache(cache, sizeof cache)) {
            plum_diag_error(diags, span, "E0909",
                            "can't find the package cache: set PLUM_HOME, or HOME (USERPROFILE "
                            "on Windows)");
            return NULL;
        }
        char dir[PATH_SIZE];
        int n = snprintf(dir, sizeof dir, "%s/%s/%s", cache, name, locked->version);
        if (n < 0 || (size_t)n >= sizeof dir) {
            plum_diag_error(diags, span, "E0909", "the package cache's path is too long");
            return NULL;
        }
        in = plum_alloc_zeroed(1, sizeof *in);
        in->name = plum_strdup(name);
        in->dir = plum_strdup(dir);
        int status = plum_manifest_load(&in->manifest, dir);
        plum_array_push(&p->installed, in);
        index = (int)p->installed.len;
        if (status > 0) {
            plum_diag_error(diags, span, "E0909",
                            "the package '%s' %s isn't installed (nothing in %s); run `plum "
                            "install`",
                            name, locked->version, dir);
            return NULL;
        }
        if (status < 0 || strcmp(in->manifest.name, name) != 0 ||
            strcmp(in->manifest.version, locked->version) != 0) {
            plum_diag_error(diags, span, "E0909",
                            "the installed package '%s' %s is damaged; delete %s and run `plum "
                            "install`",
                            name, locked->version, dir);
            return NULL;
        }
        in->ok = true;
    } else if (!in->ok) {
        return NULL; /* reported at its first seed */
    }

    /* seed weather: its lib; seed weather.client.api: client/api.plum */
    char file[PATH_SIZE];
    int n;
    if (path[strcspn(path, ".")] == '\0') {
        n = snprintf(file, sizeof file, "%s/%s", in->dir, in->manifest.lib);
    } else {
        n = snprintf(file, sizeof file, "%s/%s.plum", in->dir, path + strcspn(path, ".") + 1);
        for (char *c = file + strlen(in->dir) + 1; n > 0 && *c != '\0'; c++) {
            if (*c == '.' && strcmp(c, ".plum") != 0) {
                *c = '/';
            }
        }
    }
    if (n < 0 || (size_t)n >= sizeof file) {
        plum_diag_error(diags, span, "E0909", "the path of '%s' is too long", path);
        return NULL;
    }
    *package = index;
    return plum_strdup(file);
}
