/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Packages (spec 9.3, docs/orchard.md): the manifest plum.toml, the lock
 * file plum.lock, package names, versions and version ranges, the
 * package cache, and which paths a package may hold.
 *
 * Problems with a manifest or lock file are diagnostics with a line and
 * column (E09xx in docs/errors.md), printed like compile errors.
 */
#ifndef PLUM_ORCHARD_MANIFEST_H
#define PLUM_ORCHARD_MANIFEST_H

#include "orchard/toml.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* MAJOR.MINOR.PATCH, each a number without leading zeros. */
typedef struct PlumVersion {
    uint32_t major, minor, patch;
} PlumVersion;

bool plum_version_parse(const char *text, PlumVersion *out);
int plum_version_compare(PlumVersion a, PlumVersion b);

/* The versions from `low` (included) up to `high` (left out). */
typedef struct PlumRange {
    PlumVersion low;
    PlumVersion high;
    bool has_high;
} PlumRange;

/* "^1.2", "~1.2.3", "=1.0.0", ">=2.1", "*", or a bare "1.2" (as ^). */
bool plum_range_parse(const char *text, PlumRange *out);
bool plum_range_allows(const PlumRange *r, PlumVersion v);

/*
 * Is `name` a package name: 2 to 40 lowercase letters, digits and single
 * '-', starting with a letter and not ending with '-', and neither a
 * standard module's name nor a keyword (as it is seeded, with '_' for
 * '-')? If not, `why` (of `size` bytes) says why.
 */
bool plum_package_name_valid(const char *name, char *why, size_t size);

/* The package a `seed` name means: '_' becomes '-' (hello_orchard is
 * hello-orchard). Into `buf`; false if it doesn't fit. */
bool plum_package_from_seed(const char *seed_name, char *buf, size_t size);

/*
 * Why `path` can't be a file in a package (a reason), or NULL if it can:
 * a relative path with '/' between parts, each part letters, digits, '_',
 * '-' and '.', no "." or "..", no drive letters or device names, and a
 * file of a package's kind (.plum anywhere; plum.toml, README.md and
 * LICENSE* at the top).
 */
const char *plum_package_path_problem(const char *path);

typedef struct PlumDependency {
    const char *name;
    const char *range;
    PlumSpan span;
} PlumDependency;

typedef struct PlumManifest {
    char *path; /* DIR/plum.toml */
    char *text;
    PlumSource source;
    PlumDiagList diags;
    PlumToml toml;
    const char *name, *version, *entry, *lib; /* lib defaults to entry */
    const char *description, *license, *repository, *plum;
    const char *const *authors;
    size_t author_count;
    const char *const *keywords;
    size_t keyword_count;
    PLUM_ARRAY(PlumDependency) deps;
} PlumManifest;

/*
 * Read and check DIR/plum.toml. Returns 0 when it is a good manifest, -1
 * when it has problems (in m->diags), or an errno code when it can't be
 * read. Free with plum_manifest_free in every case.
 */
int plum_manifest_load(PlumManifest *m, const char *dir);
void plum_manifest_free(PlumManifest *m);
const PlumDependency *plum_manifest_dep(const PlumManifest *m, const char *name);

typedef struct PlumLockEntry {
    const char *name;
    const char *version;
    const char *sha256;
    const char *const *deps;
    size_t dep_count;
} PlumLockEntry;

typedef struct PlumLock {
    char *path; /* DIR/plum.lock */
    char *text;
    PlumSource source;
    PlumDiagList diags;
    PlumToml toml;
    PLUM_ARRAY(PlumLockEntry) entries;
} PlumLock;

/* Read and check DIR/plum.lock, like plum_manifest_load. */
int plum_lock_load(PlumLock *l, const char *dir);
void plum_lock_free(PlumLock *l);
const PlumLockEntry *plum_lock_find(const PlumLock *l, const char *name);

/*
 * The entry file of the package in `dir` (plum_alloc'd), for `plum run
 * dir`; NULL after printing to `err` why there is none.
 */
char *plum_package_entry(const char *dir, FILE *err);

/*
 * The directory packages are unpacked in, with '/' separators:
 * $PLUM_HOME/orchard, else ~/.plum/orchard ($HOME), or on Windows
 * %LOCALAPPDATA%\plum\orchard (else %USERPROFILE%\.plum\orchard). False if
 * none of those is set or it doesn't fit in `buf`.
 */
bool plum_orchard_cache(char *buf, size_t size);

#endif /* PLUM_ORCHARD_MANIFEST_H */
