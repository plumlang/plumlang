/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The semver module: semantic versions (https://semver.org, 2.0.0) and
 * the version ranges plum.toml uses. The parsing, ordering and ranges are
 * Orchard's own (src/util/semver.c), so a range means the same here as in
 * a manifest; this module adds prereleases and build metadata.
 */
#include "stdlib/native.h"

#include "util/alloc.h"
#include "util/semver.h"
#include "vm/table.h"

#include <stdio.h>
#include <string.h>

#define ARGS PlumNative *n, const PlumValue *args, int argc, PlumValue *result

/* The text, quoted for a message: at most 60 bytes of it. */
#define SHOWN(s) (s)->len > 60 ? 60 : (int)(s)->len, (s)->chars, (s)->len > 60 ? "..." : ""

static bool read_version(PlumNative *n, const char *fn, PlumValue arg, PlumVersion *out) {
    const PlumString *s = plum_as_string(arg);
    if (plum_semver_parse(s->chars, s->len, out)) {
        return true;
    }
    return plum_native_throw(n, PLUM_ERROR_VALUE,
                             "semver.%s: \"%.*s%s\" isn't a version; write MAJOR.MINOR.PATCH, "
                             "like \"1.4.2\" or \"2.0.0-beta.1\"",
                             fn, SHOWN(s));
}

static bool read_range(PlumNative *n, const char *fn, PlumValue arg, PlumRange *out) {
    const PlumString *s = plum_as_string(arg);
    if (plum_semver_range_parse(s->chars, s->len, true, out)) {
        return true;
    }
    return plum_native_throw(n, PLUM_ERROR_VALUE,
                             "semver.%s: \"%.*s%s\" isn't a version range; use ^1.2, ~1.2.3, "
                             "=1.2.3, >=1.2 or *",
                             fn, SHOWN(s));
}

static PlumValue number_text(uint32_t v) {
    char text[16];
    int w = snprintf(text, sizeof text, "%u", (unsigned)v);
    return plum_str_value(text, (size_t)w);
}

static void map_put(PlumTable *map, const char *key, PlumValue value) {
    plum_table_put(map, plum_str_value(key, strlen(key)), value);
}

static bool s_parse(ARGS) {
    (void)argc;
    PlumVersion v;
    if (!read_version(n, "parse", args[0], &v)) {
        return false;
    }
    PlumTable *map = plum_native_new_map(n);
    map_put(map, "major", number_text(v.major));
    map_put(map, "minor", number_text(v.minor));
    map_put(map, "patch", number_text(v.patch));
    map_put(map, "prerelease", plum_str_value(v.pre != NULL ? v.pre : "", v.pre_len));
    map_put(map, "build", plum_str_value(v.build != NULL ? v.build : "", v.build_len));
    *result = plum_obj(&map->obj);
    return true;
}

static bool s_is_valid(ARGS) {
    (void)n;
    (void)argc;
    const PlumString *s = plum_as_string(args[0]);
    PlumVersion v;
    *result = plum_bool(plum_semver_parse(s->chars, s->len, &v));
    return true;
}

static bool s_is_valid_range(ARGS) {
    (void)n;
    (void)argc;
    const PlumString *s = plum_as_string(args[0]);
    PlumRange r;
    *result = plum_bool(plum_semver_range_parse(s->chars, s->len, true, &r));
    return true;
}

static bool s_compare(ARGS) {
    (void)argc;
    PlumVersion a, b;
    if (!read_version(n, "compare", args[0], &a) || !read_version(n, "compare", args[1], &b)) {
        return false;
    }
    *result = plum_int(plum_version_compare(a, b));
    return true;
}

static bool s_satisfies(ARGS) {
    (void)argc;
    PlumVersion v;
    PlumRange r;
    if (!read_version(n, "satisfies", args[0], &v) || !read_range(n, "satisfies", args[1], &r)) {
        return false;
    }
    *result = plum_bool(plum_range_allows(&r, v));
    return true;
}

/* Every version of a list, parsed; NULL after throwing. */
static PlumVersion *read_all(PlumNative *n, const char *fn, const PlumList *list) {
    PlumVersion *all = plum_alloc_zeroed(list->len > 0 ? list->len : 1, sizeof *all);
    for (size_t i = 0; i < list->len; i++) {
        if (!read_version(n, fn, list->items[i], &all[i])) {
            plum_free(all);
            return NULL;
        }
    }
    return all;
}

/* sort(versions): oldest first; versions of equal precedence keep their order. */
static bool s_sort(ARGS) {
    (void)argc;
    const PlumList *list = plum_as_list(args[0]);
    PlumVersion *all = read_all(n, "sort", list);
    if (all == NULL) {
        return false;
    }
    size_t count = list->len;
    size_t *order = plum_alloc_zeroed(count > 0 ? count : 1, sizeof *order);
    size_t *spare = plum_alloc_zeroed(count > 0 ? count : 1, sizeof *spare);
    for (size_t i = 0; i < count; i++) {
        order[i] = i;
    }
    /* a bottom-up merge sort: stable, and O(n log n) whatever the input */
    for (size_t width = 1; width < count; width *= 2) {
        for (size_t lo = 0; lo < count; lo += 2 * width) {
            size_t mid = lo + width < count ? lo + width : count;
            size_t hi = lo + 2 * width < count ? lo + 2 * width : count;
            size_t a = lo, b = mid, k = lo;
            while (a < mid && b < hi) {
                spare[k++] = plum_version_compare(all[order[b]], all[order[a]]) < 0 ? order[b++]
                                                                                    : order[a++];
            }
            while (a < mid) {
                spare[k++] = order[a++];
            }
            while (b < hi) {
                spare[k++] = order[b++];
            }
        }
        size_t *t = order;
        order = spare;
        spare = t;
    }
    PlumList *out = plum_list_new(count);
    for (size_t i = 0; i < count; i++) {
        plum_list_push(out, plum_retain(list->items[order[i]]));
    }
    plum_free(order);
    plum_free(spare);
    plum_free(all);
    *result = plum_obj(&out->obj);
    return true;
}

/* max_satisfying(versions, range): the highest version in the range, or none. */
static bool s_max_satisfying(ARGS) {
    (void)argc;
    const PlumList *list = plum_as_list(args[0]);
    PlumRange r;
    if (!read_range(n, "max_satisfying", args[1], &r)) {
        return false;
    }
    PlumVersion *all = read_all(n, "max_satisfying", list);
    if (all == NULL) {
        return false;
    }
    size_t best = list->len;
    for (size_t i = 0; i < list->len; i++) {
        if (plum_range_allows(&r, all[i]) &&
            (best == list->len || plum_version_compare(all[i], all[best]) > 0)) {
            best = i;
        }
    }
    *result = best < list->len ? plum_retain(list->items[best]) : plum_none();
    plum_free(all);
    return true;
}

/* bump(version, part): the next major, minor or patch version. */
static bool s_bump(ARGS) {
    (void)argc;
    PlumVersion v;
    if (!read_version(n, "bump", args[0], &v)) {
        return false;
    }
    const PlumString *part = plum_as_string(args[1]);
    bool pre = v.pre != NULL;
    uint32_t major = v.major, minor = v.minor, patch = v.patch;
    if (part->len == 5 && memcmp(part->chars, "major", 5) == 0) {
        /* 2.0.0-rc.1 becomes 2.0.0: its release */
        if (!(pre && minor == 0 && patch == 0)) {
            major++;
        }
        minor = patch = 0;
    } else if (part->len == 5 && memcmp(part->chars, "minor", 5) == 0) {
        if (!(pre && patch == 0)) {
            minor++;
        }
        patch = 0;
    } else if (part->len == 5 && memcmp(part->chars, "patch", 5) == 0) {
        if (!pre) {
            patch++;
        }
    } else {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "semver.bump: \"%.*s%s\" isn't a part; bump \"major\", \"minor\" "
                                 "or \"patch\"",
                                 SHOWN(part));
    }
    if (major > PLUM_SEMVER_MAX || minor > PLUM_SEMVER_MAX || patch > PLUM_SEMVER_MAX) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "semver.bump: the %.*s number would pass %u, the largest a "
                                 "version may have",
                                 (int)part->len, part->chars, PLUM_SEMVER_MAX);
    }
    char text[40];
    int w =
        snprintf(text, sizeof text, "%u.%u.%u", (unsigned)major, (unsigned)minor, (unsigned)patch);
    *result = plum_str_value(text, (size_t)w);
    return true;
}

static const PlumNativeFunction functions[] = {
    {"parse", "(version: str) -> map[str, str]", s_parse,
     "the parts of a version: major, minor, patch, prerelease and build (\"\" when it has none)"},
    {"is_valid", "(version: str) -> bool", s_is_valid,
     "is the text a semantic version, MAJOR.MINOR.PATCH with an optional -prerelease and +build?"},
    {"compare", "(a: str, b: str) -> int", s_compare,
     "-1, 0 or 1 as a comes before, with or after b; a prerelease comes before its release, "
     "and build metadata doesn't count"},
    {"sort", "(versions: list[str]) -> list[str]", s_sort,
     "the versions, oldest first (equal ones keep their order)"},
    {"satisfies", "(version: str, range: str) -> bool", s_satisfies,
     "is the version in the range (^1.2, ~1.2.3, =1.2.3, >=1.2, *, as in plum.toml)?"},
    {"max_satisfying", "(versions: list[str], range: str) -> str?", s_max_satisfying,
     "the highest of the versions in the range, or none"},
    {"is_valid_range", "(range: str) -> bool", s_is_valid_range,
     "is the text a version range satisfies takes?"},
    {"bump", "(version: str, part: str) -> str", s_bump,
     "the next \"major\", \"minor\" or \"patch\" version; a prerelease bumps to its release"},
};

const PlumNativeModule plum_module_semver = {
    "semver",  "semantic versions and the version ranges of plum.toml",
    functions, sizeof functions / sizeof functions[0],
    NULL,      0,
};
