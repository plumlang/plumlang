/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Semantic versions (https://semver.org, 2.0.0) and version ranges: one
 * implementation for Orchard's plum.toml and plum.lock
 * (src/orchard/manifest.c, docs/orchard.md) and the semver module
 * (src/stdlib/semver.c).
 *
 * A version is MAJOR.MINOR.PATCH, each a number without leading zeros of
 * at most 9 digits, then optionally -PRERELEASE and +BUILD: dot-separated
 * identifiers of letters, digits and '-' (a numeric prerelease identifier
 * has no leading zeros). Orchard's versions are the plain ones, without
 * prerelease or build.
 *
 * A range is one requirement, as plum.toml writes them: "^1.2", "~1.2.3",
 * "=1.0.0", ">=2.1", "*", or a bare "1.2" (as ^). Parts left out are
 * zeros. With prerelease ranges allowed, a full version in a range may
 * have a prerelease ("^1.0.0-beta").
 */
#ifndef PLUM_UTIL_SEMVER_H
#define PLUM_UTIL_SEMVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The largest number a part may be: 9 digits. */
#define PLUM_SEMVER_MAX 999999999u

typedef struct PlumVersion {
    uint32_t major, minor, patch;
    const char *pre; /* the prerelease, pointing into the parsed text; NULL if none */
    size_t pre_len;
    const char *build; /* the build metadata, likewise */
    size_t build_len;
} PlumVersion;

/*
 * A full semantic version: `len` bytes of `text`, which the result points
 * into for its prerelease and build. False if it isn't one.
 */
bool plum_semver_parse(const char *text, size_t len, PlumVersion *out);

/* A plain MAJOR.MINOR.PATCH (Orchard's versions), NUL-terminated. */
bool plum_version_parse(const char *text, PlumVersion *out);

/*
 * -1, 0 or 1 by precedence: the numbers, then a version with a prerelease
 * before the same one without, prereleases identifier by identifier
 * (numbers numerically and before words, words in ASCII order, fewer
 * identifiers first). Build metadata doesn't count.
 */
int plum_version_compare(PlumVersion a, PlumVersion b);

/* The versions from `low` (included) up to `high` (left out). */
typedef struct PlumRange {
    PlumVersion low;
    PlumVersion high;
    bool has_high;
} PlumRange;

/*
 * A range of `len` bytes of `text` (which `out` may point into). With
 * `prerelease`, a full version in it may carry a prerelease (and build,
 * ignored); Orchard's ranges don't.
 */
bool plum_semver_range_parse(const char *text, size_t len, bool prerelease, PlumRange *out);

/* Orchard's ranges: NUL-terminated, no prerelease. */
bool plum_range_parse(const char *text, PlumRange *out);

/*
 * Is `v` in the range? A version with a prerelease is only when the
 * range's lower end has one too, on the same MAJOR.MINOR.PATCH (so ^1.0.0
 * never picks 2.0.0-alpha, nor 1.1.0-beta).
 */
bool plum_range_allows(const PlumRange *r, PlumVersion v);

#endif /* PLUM_UTIL_SEMVER_H */
