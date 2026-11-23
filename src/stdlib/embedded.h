/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Standard modules written in Plumlang (src/stdlib/plum), built into the
 * binary. `seed net.http` loads one like a file. embedded.c is generated
 * by tools/embed-stdlib.sh (make stdlib-embed); make test checks it's
 * up to date.
 */
#ifndef PLUM_STDLIB_EMBEDDED_H
#define PLUM_STDLIB_EMBEDDED_H

#include <stddef.h>
#include <string.h>

typedef struct PlumEmbeddedModule {
    const char *name; /* "net.http" */
    const char *text;
    size_t len;
} PlumEmbeddedModule;

/* The module `name`, or NULL. */
const PlumEmbeddedModule *plum_embedded_module(const char *name);

/* Every module, for listing; NULL past the end. */
const PlumEmbeddedModule *plum_embedded_module_at(size_t i);

#endif /* PLUM_STDLIB_EMBEDDED_H */
