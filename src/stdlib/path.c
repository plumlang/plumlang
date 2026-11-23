/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The path module: file paths as text. Nothing here touches the file
 * system; it only takes paths apart and puts them together.
 *
 * The separator is / (and on Windows \ as well, which is also what join
 * puts between parts there). A path's root (/ or, on Windows, a drive
 * such as C:\ or C:) is never taken apart: the parent of "/" is "/".
 */
#include "stdlib/native.h"

#include "util/array.h"

#include <string.h>

#define ARGS PlumNative *n, const PlumValue *args, int argc, PlumValue *result

#if defined(_WIN32)
#define SEPARATOR "\\"
#else
#define SEPARATOR "/"
#endif

typedef PLUM_ARRAY(char) Buf;

static bool is_sep(char c) {
#if defined(_WIN32)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

/* The length of the root at the start of p: a drive (Windows), then every
 * separator. 0 for a relative path. */
static size_t root_len(const char *p, size_t len) {
    size_t i = 0;
#if defined(_WIN32)
    if (len >= 2 && p[1] == ':' && ((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z'))) {
        i = 2;
    }
#endif
    while (i < len && is_sep(p[i])) {
        i++;
    }
    return i;
}

/* The end of p without separators at its end (but never into the root). */
static size_t trimmed_end(const char *p, size_t len, size_t root) {
    size_t end = len;
    while (end > root && is_sep(p[end - 1])) {
        end--;
    }
    return end;
}

/* Where the last part of p[0..end) starts. */
static size_t last_part(const char *p, size_t end, size_t root) {
    size_t start = end;
    while (start > root && !is_sep(p[start - 1])) {
        start--;
    }
    return start;
}

/* The name's extension starts at `*dot` (the '.'), or there is none. */
static bool find_extension(const char *name, size_t len, size_t *dot) {
    for (size_t i = len; i > 1; i--) { /* a dot at the start (".bashrc") doesn't count */
        if (name[i - 1] == '.') {
            if (i == len) {
                return false; /* "file.": nothing after the dot */
            }
            *dot = i - 1;
            return true;
        }
    }
    return false;
}

static void put(Buf *b, const char *s, size_t len) {
    if (len == 0) {
        return;
    }
    plum_array_reserve(b, len);
    memcpy(b->items + b->len, s, len);
    b->len += len;
}

/* join(parts): the parts with one separator between each two. */
static bool p_join(ARGS) {
    (void)argc;
    const PlumList *parts = plum_as_list(args[0]);
    Buf b = {0};
    for (size_t i = 0; i < parts->len; i++) {
        const PlumString *part = plum_as_string(parts->items[i]);
        const char *s = part->chars;
        size_t len = part->len;
        if (len == 0) {
            continue;
        }
        if (b.len > 0) {
            while (len > 0 && is_sep(s[0])) { /* no doubled separator */
                s++;
                len--;
            }
            if (!is_sep(b.items[b.len - 1])) {
                put(&b, SEPARATOR, 1);
            }
        }
        if (b.len + len > PLUM_MAX_STRING) {
            plum_array_free(&b);
            return plum_native_throw(n, PLUM_ERROR_VALUE,
                                     "path.join: the path would be longer than %zu bytes, the "
                                     "most a str can hold",
                                     PLUM_MAX_STRING);
        }
        put(&b, s, len);
    }
    *result = plum_str_value(b.items != NULL ? b.items : "", b.len);
    plum_array_free(&b);
    return true;
}

/* parent(p): p without its last part; "" for a plain name. */
static bool p_parent(ARGS) {
    (void)n;
    (void)argc;
    const PlumString *s = plum_as_string(args[0]);
    size_t root = root_len(s->chars, s->len);
    size_t end = last_part(s->chars, trimmed_end(s->chars, s->len, root), root);
    end = trimmed_end(s->chars, end, root);
    *result = plum_str_value(s->chars, end);
    return true;
}

/* The last part of s, as a start and a length. */
static size_t name_of(const PlumString *s, size_t *start) {
    size_t root = root_len(s->chars, s->len);
    size_t end = trimmed_end(s->chars, s->len, root);
    *start = last_part(s->chars, end, root);
    return end - *start;
}

static bool p_name(ARGS) {
    (void)n;
    (void)argc;
    const PlumString *s = plum_as_string(args[0]);
    size_t start = 0;
    size_t len = name_of(s, &start);
    *result = plum_str_value(s->chars + start, len);
    return true;
}

static bool p_stem(ARGS) {
    (void)n;
    (void)argc;
    const PlumString *s = plum_as_string(args[0]);
    size_t start = 0, dot = 0;
    size_t len = name_of(s, &start);
    if (find_extension(s->chars + start, len, &dot)) {
        len = dot;
    }
    *result = plum_str_value(s->chars + start, len);
    return true;
}

static bool p_extension(ARGS) {
    (void)n;
    (void)argc;
    const PlumString *s = plum_as_string(args[0]);
    size_t start = 0, dot = 0;
    size_t len = name_of(s, &start);
    if (find_extension(s->chars + start, len, &dot)) {
        *result = plum_str_value(s->chars + start + dot + 1, len - dot - 1);
    } else {
        *result = plum_str_value("", 0);
    }
    return true;
}

/* is_absolute(p): does p start at a root, not the current directory? */
static bool p_is_absolute(ARGS) {
    (void)n;
    (void)argc;
    const PlumString *s = plum_as_string(args[0]);
    size_t root = root_len(s->chars, s->len);
    /* "C:" alone, or "C:file", is relative to drive C's current directory. */
    *result = plum_bool(root > 0 && is_sep(s->chars[root - 1]));
    return true;
}

static const PlumNativeFunction functions[] = {
    {"join", "(parts: list[str]) -> str", p_join,
     "the parts joined with the separator, never doubling it; empty parts are skipped"},
    {"parent", "(p: str) -> str", p_parent,
     "the folder that holds p: \"a/b/c.txt\" gives \"a/b\", \"c.txt\" gives \"\""},
    {"name", "(p: str) -> str", p_name, "the last part: \"a/b/c.txt\" gives \"c.txt\""},
    {"stem", "(p: str) -> str", p_stem,
     "the last part without its extension: \"a/b/c.txt\" gives \"c\""},
    {"extension", "(p: str) -> str", p_extension,
     "the extension without its dot: \"a/b/c.txt\" gives \"txt\" (\"\" if none)"},
    {"is_absolute", "(p: str) -> bool", p_is_absolute,
     "does p start with a separator (on Windows, also a drive and one: C:/)?"},
};

static const PlumNativeConstant constants[] = {
    {"SEPARATOR", PLUM_NATIVE_STR, 0, 0, SEPARATOR,
     "what join puts between parts: a slash, or a backslash on Windows"},
};

const PlumNativeModule plum_module_pathlib = {
    "path",    "file paths as text: join them, take them apart",
    functions, sizeof functions / sizeof functions[0],
    constants, sizeof constants / sizeof constants[0],
};
