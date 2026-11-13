/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/* The fs module: files and directories. Failures throw IOError with the
 * operating system's reason. */
#include "stdlib/native.h"

#include "stdlib/platform.h"
#include "util/array.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARGS PlumNative *n, const PlumValue *args, int argc, PlumValue *result

/* A path argument, refusing zero bytes (they would cut it short). */
static const char *path_of(PlumNative *n, const char *what, PlumValue v) {
    const PlumString *s = plum_as_string(v);
    if (s->len == 0) {
        plum_native_throw(n, PLUM_ERROR_IO, "%s(\"\"): the path is empty", what);
        return NULL;
    }
    if (memchr(s->chars, '\0', s->len) != NULL) {
        plum_native_throw(n, PLUM_ERROR_IO, "%s(): the path contains a zero byte", what);
        return NULL;
    }
    return s->chars;
}

static bool io_fail(PlumNative *n, const char *what, const char *path, int err) {
    return plum_native_throw(n, PLUM_ERROR_IO, "%s(\"%s\"): %s", what, path, strerror(err));
}

static bool fs_read(ARGS) {
    (void)argc;
    const char *path = path_of(n, "read", args[0]);
    if (path == NULL) {
        return false;
    }
    if (plum_os_is_dir(path)) {
        return plum_native_throw(n, PLUM_ERROR_IO, "read(\"%s\"): this is a directory", path);
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return io_fail(n, "read", path, errno);
    }
    PLUM_ARRAY(char) text = {0};
    char chunk[8192];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) {
        for (size_t i = 0; i < got; i++) {
            plum_array_push(&text, chunk[i]);
        }
    }
    bool failed = ferror(f) != 0;
    fclose(f);
    if (failed) {
        plum_array_free(&text);
        return plum_native_throw(n, PLUM_ERROR_IO, "read(\"%s\"): the file couldn't be read", path);
    }
    *result = plum_str_value(text.items != NULL ? text.items : "", text.len);
    plum_array_free(&text);
    return true;
}

static bool put_file(PlumNative *n, const char *what, const char *mode, const PlumValue *args,
                     PlumValue *result) {
    const char *path = path_of(n, what, args[0]);
    if (path == NULL) {
        return false;
    }
    const PlumString *text = plum_as_string(args[1]);
    FILE *f = fopen(path, mode);
    if (f == NULL) {
        return io_fail(n, what, path, errno);
    }
    size_t wrote = fwrite(text->chars, 1, text->len, f);
    bool ok = wrote == text->len && fclose(f) == 0;
    if (!ok) {
        return plum_native_throw(n, PLUM_ERROR_IO, "%s(\"%s\"): the file couldn't be written", what,
                                 path);
    }
    *result = plum_none();
    return true;
}

static bool fs_write(ARGS) {
    (void)argc;
    return put_file(n, "write", "wb", args, result);
}

static bool fs_append(ARGS) {
    (void)argc;
    return put_file(n, "append", "ab", args, result);
}

static bool fs_exists(ARGS) {
    (void)argc;
    const PlumString *s = plum_as_string(args[0]);
    bool found = false;
    if (s->len > 0 && memchr(s->chars, '\0', s->len) == NULL) {
        FILE *f = fopen(s->chars, "rb");
        found = f != NULL || plum_os_is_dir(s->chars);
        if (f != NULL) {
            fclose(f);
        }
    }
    (void)n;
    *result = plum_bool(found);
    return true;
}

static bool fs_is_dir(ARGS) {
    (void)n;
    (void)argc;
    const PlumString *s = plum_as_string(args[0]);
    *result =
        plum_bool(s->len > 0 && memchr(s->chars, '\0', s->len) == NULL && plum_os_is_dir(s->chars));
    return true;
}

static bool fs_remove(ARGS) {
    (void)argc;
    const char *path = path_of(n, "remove", args[0]);
    if (path == NULL) {
        return false;
    }
    int err = plum_os_is_dir(path) ? plum_os_remove_dir(path) : (remove(path) == 0 ? 0 : errno);
    if (err != 0) {
        return io_fail(n, "remove", path, err);
    }
    *result = plum_none();
    return true;
}

static bool fs_rename(ARGS) {
    (void)argc;
    const char *from = path_of(n, "rename", args[0]);
    const char *to = from != NULL ? path_of(n, "rename", args[1]) : NULL;
    if (to == NULL) {
        return false;
    }
    if (rename(from, to) != 0) {
        return io_fail(n, "rename", from, errno);
    }
    *result = plum_none();
    return true;
}

static bool fs_make_dir(ARGS) {
    (void)argc;
    const char *path = path_of(n, "make_dir", args[0]);
    if (path == NULL) {
        return false;
    }
    int err = plum_os_make_dir(path);
    if (err != 0 && !(err == EEXIST && plum_os_is_dir(path))) {
        return io_fail(n, "make_dir", path, err);
    }
    *result = plum_none();
    return true;
}

static void add_name(void *ctx, const char *name) {
    plum_list_push((PlumList *)ctx, plum_str_value(name, strlen(name)));
}

static int by_text(const void *a, const void *b) {
    const PlumString *x = plum_as_string(*(const PlumValue *)a);
    const PlumString *y = plum_as_string(*(const PlumValue *)b);
    size_t len = x->len < y->len ? x->len : y->len;
    int c = memcmp(x->chars, y->chars, len);
    return c != 0 ? c : (x->len > y->len) - (x->len < y->len);
}

/* list(dir): the names in a directory, sorted. */
static bool fs_list(ARGS) {
    (void)argc;
    const char *path = path_of(n, "list", args[0]);
    if (path == NULL) {
        return false;
    }
    PlumList *out = plum_list_new(0);
    int err = plum_os_list_dir(path, add_name, out);
    if (err != 0) {
        plum_release(plum_obj(&out->obj));
        return io_fail(n, "list", path, err);
    }
    if (out->len > 1) {
        qsort(out->items, out->len, sizeof(PlumValue), by_text);
    }
    *result = plum_obj(&out->obj);
    return true;
}

static bool fs_temp_dir(ARGS) {
    (void)n;
    (void)args;
    (void)argc;
    char buf[4096];
    plum_os_temp_dir(buf, sizeof buf);
    *result = plum_str_value(buf, strlen(buf));
    return true;
}

static const PlumNativeFunction functions[] = {
    {"read", "(path: str) -> str", fs_read, "the whole file as text"},
    {"write", "(path: str, text: str)", fs_write, "make or replace the file with text"},
    {"append", "(path: str, text: str)", fs_append, "add text to the end of the file"},
    {"exists", "(path: str) -> bool", fs_exists, "is there a file or directory at path?"},
    {"is_dir", "(path: str) -> bool", fs_is_dir, "is path a directory?"},
    {"remove", "(path: str)", fs_remove, "delete a file, or an empty directory"},
    {"rename", "(from: str, to: str)", fs_rename, "move or rename a file or directory"},
    {"make_dir", "(path: str)", fs_make_dir, "make a directory (fine if it already exists)"},
    {"list", "(path: str) -> list[str]", fs_list, "the names in a directory, sorted"},
    {"temp_dir", "() -> str", fs_temp_dir, "a directory for temporary files"},
};

const PlumNativeModule plum_module_fs = {
    "fs", "files and directories", functions, sizeof functions / sizeof functions[0], NULL, 0,
};
