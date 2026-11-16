/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Finding the file a `seed` of a package means (spec 9.3). The program's
 * project is the nearest directory, from the program's file upwards, with
 * a plum.toml; its plum.lock says which version of each package to use,
 * and the package cache (plum_orchard_cache) holds them unpacked. A
 * module may seed only the packages its own plum.toml lists: the
 * project's, or, inside an installed package, that package's.
 */
#ifndef PLUM_ORCHARD_PACKAGES_H
#define PLUM_ORCHARD_PACKAGES_H

#include "diag/diag.h"

#include <stdbool.h>

typedef struct PlumPackages PlumPackages;

void plum_packages_free(PlumPackages *p);

/*
 * Is `seed path` (not a relative path) a package rather than a standard
 * module? Standard modules win: a name whose first part is a standard
 * module's (net.whatever) is never a package.
 */
bool plum_seed_is_package(const char *path);

/*
 * The file `seed path` means for a module of `from_package` (0: the
 * project, n: the nth installed package) at `from_path`: a new path
 * (plum_alloc'd), with *package set to the package the file belongs to.
 * NULL after reporting why not (E0908, E0909, or the manifest's error) at
 * `span` in `diags`. `*p` is made on first use.
 */
char *plum_packages_find(PlumPackages **p, const char *from_path, int from_package,
                         const char *path, PlumDiagList *diags, PlumSpan span, int *package);

#endif /* PLUM_ORCHARD_PACKAGES_H */
