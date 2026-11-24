/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The `plum test` command (spec 14.3, docs/test.md):
 *
 *   plum test [paths...] [--filter TEXT] [--json]
 *
 * A test file is a file whose name ends in _test.plum (directories are
 * searched for them; a file named on the command line is a test file
 * whatever its name). A test is a top-level `branch test_...()` with no
 * parameters and no result.
 *
 * Each test runs on its own: the file is loaded, checked and compiled
 * again with a call to the test after its last line, and run in a fresh
 * VM, so the module's top-level code runs before every test and nothing
 * the test changes reaches the next one. What the test prints is kept and
 * shown only if it fails.
 */
#include "cli/test.h"

#include "cli/pipeline.h"
#include "diag/diag.h"
#include "module/loader.h"
#include "parser/ast.h"
#include "stdlib/platform.h"
#include "util/alloc.h"
#include "util/array.h"
#include "vm/vm.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    EXIT_OK = 0,
    EXIT_FAILED = 1,
    EXIT_USAGE = 2,
    EXIT_NOINPUT = 3,
};

/* Directories nested deeper than this are left alone (a symlink loop). */
#define MAX_DEPTH 64

typedef PLUM_ARRAY(char *) Strings;

typedef struct TestRun {
    const char *filter; /* run only the tests whose name contains this */
    bool json;
    int passed;
    int failed;
    int broken; /* test files with errors, so none of their tests ran */
    int status;
    bool first_json; /* nothing written into "tests" yet */
    Strings broken_files;
} TestRun;

static void fail(TestRun *run, int status) {
    if (status > run->status) {
        run->status = status;
    }
}

/* ------------------------------------------------------------------ */
/* Finding the test files                                              */
/* ------------------------------------------------------------------ */

static void add_name(void *ctx, const char *name) {
    plum_array_push((Strings *)ctx, plum_strdup(name));
}

static int compare_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static bool ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s);
    size_t m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

/* Every NAME_test.plum under `dir`, in name order; hidden entries are
 * skipped. */
static void find_tests(TestRun *run, const char *dir, int depth, Strings *files) {
    if (depth > MAX_DEPTH) {
        return;
    }
    Strings names = {0};
    int err = plum_os_list_dir(dir, add_name, &names);
    if (err != 0) {
        fprintf(stderr, "plum: error: cannot read '%s': %s\n", dir, strerror(err));
        fail(run, EXIT_NOINPUT);
    }
    if (names.len > 1) {
        qsort(names.items, names.len, sizeof(char *), compare_names);
    }
    size_t dlen = strlen(dir);
    while (dlen > 1 && (dir[dlen - 1] == '/' || dir[dlen - 1] == '\\')) {
        dlen--;
    }
    bool here = dlen == 1 && dir[0] == '.'; /* "./a_test.plum" reads as "a_test.plum" */
    for (size_t i = 0; i < names.len; i++) {
        char *name = names.items[i];
        if (name[0] == '.') {
            plum_free(name);
            continue;
        }
        char *path;
        if (here) {
            path = name;
        } else {
            size_t size = plum_add_size(plum_add_size(dlen, strlen(name)), 2);
            path = plum_alloc(size);
            snprintf(path, size, "%.*s/%s", (int)dlen, dir, name);
            plum_free(name);
        }
        if (plum_os_is_dir(path)) {
            find_tests(run, path, depth + 1, files);
            plum_free(path);
        } else if (ends_with(path, "_test.plum")) {
            plum_array_push(files, path);
        } else {
            plum_free(path);
        }
    }
    plum_array_free(&names);
}

/* ------------------------------------------------------------------ */
/* Capturing what a test prints                                        */
/* ------------------------------------------------------------------ */

typedef struct Capture {
    FILE *file;
    char *path; /* when it isn't a tmpfile(), to remove afterwards */
} Capture;

static bool capture_open(Capture *c) {
    c->path = NULL;
    c->file = tmpfile();
    if (c->file != NULL) {
        return true;
    }
    /* tmpfile() can fail where the C library puts its files somewhere
     * unwritable (the root of the drive, on older Windows runtimes): a
     * file of our own in the temporary directory instead. */
    char dir[1024];
    plum_os_temp_dir(dir, sizeof dir);
    unsigned long salt = (unsigned long)time(NULL) ^ (unsigned long)(uintptr_t)c;
    for (int i = 0; i < 100 && c->file == NULL; i++) {
        char path[1100];
        snprintf(path, sizeof path, "%s/plum-test-%lx-%d.tmp", dir, salt, i);
        FILE *existing = fopen(path, "rb");
        if (existing != NULL) {
            fclose(existing);
            continue;
        }
        c->file = fopen(path, "w+b");
        if (c->file != NULL) {
            c->path = plum_strdup(path);
        }
    }
    return c->file != NULL;
}

/* Everything written to the capture, NUL-terminated (plum_free it). */
static char *capture_text(Capture *c) {
    fflush(c->file);
    long size = 0;
    if (fseek(c->file, 0, SEEK_END) == 0) {
        size = ftell(c->file);
    }
    if (size < 0) {
        size = 0;
    }
    char *text = plum_alloc(plum_add_size((size_t)size, 1));
    rewind(c->file);
    size_t got = fread(text, 1, (size_t)size, c->file);
    text[got] = '\0';
    return text;
}

static void capture_close(Capture *c) {
    if (c->file != NULL) {
        fclose(c->file);
    }
    if (c->path != NULL) {
        remove(c->path);
        plum_free(c->path);
    }
}

/* ------------------------------------------------------------------ */
/* Running one test                                                    */
/* ------------------------------------------------------------------ */

typedef struct Test {
    const char *name;
    unsigned line;
    bool well_formed; /* no parameters and no result */
} Test;

typedef struct Outcome {
    bool passed;
    char *output; /* what it printed */
    char *errors; /* what went wrong, as the runtime reported it */
    int code;     /* its exit code */
} Outcome;

/* Run `test` from the file `path`, whose text is `text`. */
static void run_test(const char *path, const char *text, size_t len, const Test *test,
                     Outcome *result) {
    result->passed = false;
    result->output = NULL;
    result->errors = NULL;
    result->code = 1;

    /* The file as it is, then `\ntest_name()\n`: the lines above the call
     * keep their numbers. */
    size_t name_len = strlen(test->name);
    size_t full_len = plum_add_size(plum_add_size(len, name_len), 4);
    char *full = malloc(full_len + 1); /* the loader frees it with free() */
    if (full == NULL) {
        plum_fatal("out of memory");
    }
    memcpy(full, text, len);
    snprintf(full + len, full_len + 1 - len, "\n%s()\n", test->name);

    Capture out = {0};
    Capture err = {0};
    if (!capture_open(&out) || !capture_open(&err)) {
        free(full);
        capture_close(&out);
        capture_close(&err);
        result->errors = plum_strdup("plum: cannot make a temporary file for the test's output\n");
        return;
    }

    PlumModules modules;
    plum_modules_load_text(&modules, path, full, full_len);
    bool ok = plum_cli_check(&modules);
    PlumCompiled compiled = {0};
    if (ok) {
        compiled = plum_cli_compile(&modules);
    }
    if (compiled.main == NULL) {
        /* The file checked on its own, so this is rare: report it all. */
        plum_modules_print_diags(&modules, err.file, false);
    } else {
        PlumModule *program = modules.items[modules.count - 1];
        char *argv[1] = {(char *)path};
        PlumRunOptions options = {0};
        options.argc = 1;
        options.argv = argv;
        result->code = plum_vm_run(&compiled, &program->source, &options, out.file, err.file);
        result->passed = result->code == 0;
    }
    plum_compiled_free(&compiled);
    plum_modules_free(&modules);

    result->output = capture_text(&out);
    result->errors = capture_text(&err);
    capture_close(&out);
    capture_close(&err);
}

/* ------------------------------------------------------------------ */
/* Reading a runtime error's report                                    */
/* ------------------------------------------------------------------ */

/*
 * A runtime error reads (src/vm/vm.c):
 *
 *     PATH:LINE: TREE: MESSAGE
 *       in FUNCTION, line N            (a frame in the program's own file)
 *       in FUNCTION, line N of PATH    (a frame in another file)
 *       ... K more calls
 *
 * Failure is that, taken apart. The place reported is where the error was
 * thrown, unless that is inside the standard library (test.throws): then
 * it is the line of the caller's code.
 */
typedef struct Failure {
    char *tree;
    char *message;
    char *file;
    unsigned line;
    char *frames; /* the frame lines, without the generated call's */
} Failure;

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

/* Parse the first frame-like location in `line` ("  in F, line N[ of P]"). */
static bool parse_frame(const char *line, size_t len, const char *own, char **file,
                        unsigned *number) {
    const char *at = NULL;
    for (size_t i = 0; i + 7 <= len; i++) {
        if (memcmp(line + i, ", line ", 7) == 0) {
            at = line + i + 7;
        }
    }
    if (at == NULL || !is_digit(*at)) {
        return false;
    }
    unsigned long n = strtoul(at, NULL, 10);
    while (at < line + len && is_digit(*at)) {
        at++;
    }
    *number = (unsigned)n;
    if ((size_t)(line + len - at) > 4 && memcmp(at, " of ", 4) == 0) {
        *file = plum_strndup(at + 4, (size_t)(line + len - at - 4));
    } else {
        *file = plum_strdup(own);
    }
    return true;
}

static bool parse_failure(const char *errors, const char *own, unsigned call_line, Failure *f) {
    memset(f, 0, sizeof *f);
    /* PATH:LINE: */
    const char *p = errors;
    const char *colon = NULL;
    for (const char *s = errors; *s != '\0' && *s != '\n'; s++) {
        if (s[0] == ':' && is_digit(s[1])) {
            const char *d = s + 1;
            while (is_digit(*d)) {
                d++;
            }
            if (d[0] == ':' && d[1] == ' ') {
                colon = s;
                break;
            }
        }
    }
    if (colon == NULL) {
        return false;
    }
    char *file = plum_strndup(p, (size_t)(colon - p));
    unsigned line = (unsigned)strtoul(colon + 1, NULL, 10);
    p = strchr(colon + 1, ':') + 2;
    /* TREE: */
    const char *end_tree = strstr(p, ": ");
    const char *eol = strchr(p, '\n');
    if (end_tree == NULL || (eol != NULL && end_tree > eol)) {
        plum_free(file);
        return false;
    }
    f->tree = plum_strndup(p, (size_t)(end_tree - p));
    p = end_tree + 2;
    /* MESSAGE, up to the first frame line */
    const char *frames = p;
    for (;;) {
        const char *nl = strchr(frames, '\n');
        if (nl == NULL) {
            frames += strlen(frames);
            break;
        }
        frames = nl + 1;
        if (strncmp(frames, "  in ", 5) == 0 || strncmp(frames, "  ... ", 6) == 0) {
            break;
        }
    }
    size_t mlen = (size_t)(frames - p);
    while (mlen > 0 && p[mlen - 1] == '\n') {
        mlen--;
    }
    f->message = plum_strndup(p, mlen);
    f->file = file;
    f->line = line;

    /* The frames, but for the call this runner added at the bottom. */
    PLUM_ARRAY(char) kept = {0};
    bool placed = strncmp(file, "<stdlib", 7) != 0;
    for (const char *s = frames; *s != '\0';) {
        const char *nl = strchr(s, '\n');
        size_t n = nl != NULL ? (size_t)(nl - s) : strlen(s);
        char *frame_file = NULL;
        unsigned number = 0;
        bool is_frame = parse_frame(s, n, own, &frame_file, &number);
        bool generated = is_frame && strncmp(s, "  in <main>, line ", 18) == 0 &&
                         number == call_line && strcmp(frame_file, own) == 0;
        if (!generated) {
            plum_array_reserve(&kept, n + 1);
            memcpy(kept.items + kept.len, s, n);
            kept.len += n;
            kept.items[kept.len++] = '\n';
            if (!placed && is_frame && strncmp(frame_file, "<stdlib", 7) != 0) {
                plum_free(f->file);
                f->file = frame_file;
                f->line = number;
                frame_file = NULL;
                placed = true;
            }
        }
        plum_free(frame_file);
        s += n;
        if (*s == '\n') {
            s++;
        }
    }
    plum_array_push(&kept, '\0');
    f->frames = kept.items;
    return true;
}

static void failure_free(Failure *f) {
    plum_free(f->tree);
    plum_free(f->message);
    plum_free(f->file);
    plum_free(f->frames);
}

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

static void json_string(const char *s) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        switch (*p) {
        case '"':
            fputs("\\\"", stdout);
            break;
        case '\\':
            fputs("\\\\", stdout);
            break;
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\r':
            fputs("\\r", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        default:
            if (*p < 0x20) {
                printf("\\u%04x", *p);
            } else {
                putchar(*p);
            }
        }
    }
    putchar('"');
}

/* Each line of `text` with `indent` before it. */
static void print_indented(const char *text, const char *indent) {
    while (*text != '\0') {
        const char *nl = strchr(text, '\n');
        size_t n = nl != NULL ? (size_t)(nl - text) : strlen(text);
        printf("%s%.*s\n", indent, (int)n, text);
        text += n;
        if (*text == '\n') {
            text++;
        }
    }
}

static void report(TestRun *run, const char *path, const Test *test, const Outcome *o,
                   unsigned call_line) {
    Failure f;
    bool parsed = !o->passed && o->errors != NULL && parse_failure(o->errors, path, call_line, &f);
    /* Without a runtime error's report: exit(n), a task's failure, ... */
    char exited[64];
    snprintf(exited, sizeof exited, "the test called exit(%d)", o->code);
    const char *plain = o->errors != NULL && o->errors[0] != '\0' ? o->errors : exited;

    if (run->json) {
        printf("%s\n    {\"file\": ", run->first_json ? "" : ",");
        run->first_json = false;
        json_string(path);
        printf(", \"name\": ");
        json_string(test->name);
        printf(", \"line\": %u, \"passed\": %s", test->line, o->passed ? "true" : "false");
        if (!o->passed) {
            printf(", \"error\": {");
            if (parsed) {
                printf("\"type\": ");
                json_string(f.tree);
                printf(", \"message\": ");
                json_string(f.message);
                printf(", \"file\": ");
                json_string(f.file);
                printf(", \"line\": %u, \"trace\": ", f.line);
                json_string(f.frames);
            } else {
                printf("\"message\": ");
                json_string(plain);
            }
            printf("}, \"output\": ");
            json_string(o->output != NULL ? o->output : "");
        }
        printf("}");
    } else if (o->passed) {
        printf("  ok   %s\n", test->name);
    } else {
        printf("  FAIL %s\n", test->name);
        if (parsed) {
            printf("       %s:%u: %s: ", f.file, f.line, f.tree);
            const char *msg = f.message;
            const char *nl = strchr(msg, '\n');
            printf("%.*s\n", (int)(nl != NULL ? (size_t)(nl - msg) : strlen(msg)), msg);
            if (nl != NULL) {
                print_indented(nl + 1, "       ");
            }
            print_indented(f.frames, "       ");
        } else {
            print_indented(plain, "       ");
        }
        if (o->output != NULL && o->output[0] != '\0') {
            printf("       output:\n");
            print_indented(o->output, "         ");
        }
    }
    if (parsed) {
        failure_free(&f);
    }
}

/* ------------------------------------------------------------------ */
/* One file                                                            */
/* ------------------------------------------------------------------ */

static unsigned count_lines(const char *text, size_t len) {
    unsigned n = 0;
    for (size_t i = 0; i < len; i++) {
        n += text[i] == '\n';
    }
    return n;
}

static void test_file(TestRun *run, const char *path) {
    size_t len = 0;
    int err = 0;
    char *text = plum_read_file(path, &len, &err);
    if (text == NULL) {
        fprintf(stderr, "plum: error: cannot read '%s': %s\n", path, strerror(err));
        fail(run, EXIT_NOINPUT);
        return;
    }

    /* Check the file as it is: its errors and warnings are reported once,
     * and its tests are found in its syntax tree. */
    PlumModules modules;
    err = plum_modules_load(&modules, path);
    if (err != 0) {
        fprintf(stderr, "plum: error: cannot read '%s': %s\n", path, strerror(err));
        fail(run, EXIT_NOINPUT);
        free(text);
        return;
    }
    bool ok = plum_cli_check(&modules);
    fflush(stdout);
    plum_modules_print_diags(&modules, stderr, plum_diag_use_color(stderr));
    fflush(stderr);
    PLUM_ARRAY(Test) tests = {0};
    if (ok) {
        const PlumProgram *program = &modules.items[modules.count - 1]->program;
        for (size_t i = 0; i < program->count; i++) {
            const PlumStmt *s = program->stmts[i];
            if (s->kind != PLUM_STMT_BRANCH || strncmp(s->as.branch.name, "test_", 5) != 0) {
                continue;
            }
            if (run->filter != NULL && strstr(s->as.branch.name, run->filter) == NULL) {
                continue;
            }
            Test t = {plum_strdup(s->as.branch.name), (unsigned)s->span.line,
                      s->as.branch.param_count == 0 && s->as.branch.result == NULL};
            plum_array_push(&tests, t);
        }
    }
    plum_modules_free(&modules);

    if (!ok) {
        run->broken++;
        plum_array_push(&run->broken_files, plum_strdup(path));
        fail(run, EXIT_FAILED);
        if (!run->json) {
            printf("%s\n  FAIL it has errors, so its tests did not run\n", path);
        }
    } else if (tests.len > 0 && !run->json) {
        printf("%s\n", path);
    }
    fflush(stdout);

    unsigned call_line = count_lines(text, len) + 2;
    for (size_t i = 0; i < tests.len; i++) {
        const Test *t = &tests.items[i];
        Outcome o = {0};
        if (t->well_formed) {
            run_test(path, text, len, t, &o);
        } else {
            char msg[512];
            snprintf(msg, sizeof msg, "%s:%u: a test takes no parameters and returns nothing\n",
                     path, t->line);
            o.errors = plum_strdup(msg);
            o.code = 1;
        }
        if (o.passed) {
            run->passed++;
        } else {
            run->failed++;
            fail(run, EXIT_FAILED);
        }
        report(run, path, t, &o, call_line);
        fflush(stdout);
        plum_free(o.output);
        plum_free(o.errors);
        plum_free((char *)t->name);
    }
    plum_array_free(&tests);
    free(text);
}

/* ------------------------------------------------------------------ */
/* The command                                                         */
/* ------------------------------------------------------------------ */

static int usage(const char *problem, const char *arg) {
    fprintf(stderr,
            "plum: error: %s '%s'\n"
            "  hint: plum test [files or directories...] [--filter TEXT] [--json]\n",
            problem, arg);
    return EXIT_USAGE;
}

int plum_test_command(int argc, char **argv) {
    TestRun run = {0};
    run.first_json = true;
    Strings paths = {0};
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--json") == 0) {
            run.json = true;
        } else if (strcmp(a, "--filter") == 0) {
            if (i + 1 >= argc) {
                plum_array_free(&paths);
                return usage("give the text to look for in test names after", a);
            }
            run.filter = argv[++i];
        } else if (strncmp(a, "--filter=", 9) == 0) {
            run.filter = a + 9;
        } else if (a[0] == '-' && a[1] != '\0') {
            plum_array_free(&paths);
            return usage("unknown option", a);
        } else {
            plum_array_push(&paths, (char *)a);
        }
    }

    Strings files = {0};
    if (paths.len == 0) {
        find_tests(&run, ".", 0, &files);
    }
    for (size_t i = 0; i < paths.len; i++) {
        if (plum_os_is_dir(paths.items[i])) {
            find_tests(&run, paths.items[i], 0, &files);
        } else {
            /* Named on the command line: a test file whatever its name. */
            plum_array_push(&files, plum_strdup(paths.items[i]));
        }
    }
    plum_array_free(&paths);

    if (run.json) {
        printf("{\n  \"tests\": [");
    }
    for (size_t i = 0; i < files.len; i++) {
        test_file(&run, files.items[i]);
        plum_free(files.items[i]);
    }
    plum_array_free(&files);

    if (run.json) {
        printf("%s],\n  \"broken\": [", run.first_json ? "" : "\n  ");
        for (size_t i = 0; i < run.broken_files.len; i++) {
            printf("%s", i > 0 ? ", " : "");
            json_string(run.broken_files.items[i]);
        }
        printf("],\n  \"passed\": %d,\n  \"failed\": %d\n}\n", run.passed, run.failed);
    } else if (run.passed + run.failed + run.broken == 0) {
        if (run.filter != NULL) {
            printf("no tests match '%s'\n", run.filter);
        } else {
            printf("no tests found: a test is a `branch test_...()` in a file named "
                   "NAME_test.plum\n");
        }
    } else {
        printf("\n%d passed, %d failed", run.passed, run.failed);
        if (run.broken > 0) {
            printf(", %d %s with errors", run.broken, run.broken == 1 ? "file" : "files");
        }
        printf("\n");
    }
    for (size_t i = 0; i < run.broken_files.len; i++) {
        plum_free(run.broken_files.items[i]);
    }
    plum_array_free(&run.broken_files);
    fflush(stdout);
    return run.status;
}
