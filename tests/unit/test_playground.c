/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/* The browser playground's core (src/wasm/playground.h), run natively. */
#include "check.h"
#include "stdlib/platform.h"
#include "suites.h"
#include "util/alloc.h"
#include "wasm/playground.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Run `src`; its output and errors go into `out` and `err` (each `size` bytes). */
static int run(const char *src, const PlumPlaygroundLimits *limits, char *out, char *err,
               size_t size) {
    char dir[512], out_path[600], err_path[600];
    plum_os_temp_dir(dir, sizeof dir);
    snprintf(out_path, sizeof out_path, "%s/plum-playground-test.out", dir);
    snprintf(err_path, sizeof err_path, "%s/plum-playground-test.err", dir);
    FILE *o = fopen(out_path, "w+b");
    FILE *e = fopen(err_path, "w+b");
    int code = -1;
    out[0] = err[0] = '\0';
    if (o != NULL && e != NULL) {
        code = plum_playground_run(src, strlen(src), limits, o, e);
        rewind(o);
        rewind(e);
        out[fread(out, 1, size - 1, o)] = '\0';
        err[fread(err, 1, size - 1, e)] = '\0';
    }
    if (o != NULL) {
        fclose(o);
    }
    if (e != NULL) {
        fclose(e);
    }
    remove(out_path);
    remove(err_path);
    return code;
}

static void test_run(void) {
    char out[2048], err[2048];
    CHECK(run("drop(\"hi\", 1 + 2)\n", NULL, out, err, sizeof out) == 0);
    CHECK_STR(out, "hi 3\n");
    CHECK_STR(err, "");

    /* An error in the program: a message on err, exit code 1. */
    CHECK(run("plant xs = [1]\ndrop(xs[5])\n", NULL, out, err, sizeof out) == 1);
    CHECK(strstr(err, "out of range") != NULL);

    /* A type error: no run, the diagnostic on err. */
    CHECK(run("plant x: int = \"a\"\n", NULL, out, err, sizeof out) == 1);
    CHECK(strstr(err, "main.plum:1:") != NULL);

    /* An endless loop stops at the step limit. */
    PlumPlaygroundLimits few = {1000, 0, 0};
    CHECK(run("plant i = 0\nwhile good {\n    i += 1\n}\n", &few, out, err, sizeof out) == 1);
    CHECK(strstr(err, "stopped after 1000 steps") != NULL);

    /* A while loop that tests its condition at the bottom (#123) counts a
     * step each time round, as the jump back to its test did: the calls
     * of the program and of f are steps 1 and 2, and going round after
     * drop(8) is step 11, past the limit. */
    PlumPlaygroundLimits ten = {10, 0, 0};
    CHECK(run("branch f() {\n    plant i = 0\n    while i < 100 {\n        drop(i)\n"
              "        i += 1\n    }\n}\nf()\n",
              &ten, out, err, sizeof out) == 1);
    CHECK_STR(out, "0\n1\n2\n3\n4\n5\n6\n7\n8\n");
    CHECK(strstr(err, "stopped after 10 steps") != NULL);

    /* So does a list that grows without end, at the memory limit. */
    PlumPlaygroundLimits small = {0, 1 << 20, 0};
    CHECK(run("plant xs = [\"x\"]\nwhile good {\n    xs.push(\"plum\".repeat(100))\n}\n", &small,
              out, err, sizeof out) == 1);
    CHECK(strstr(err, "stopped using more than") != NULL);

    /* The sandbox: no files. */
    CHECK(run("seed fs\ndrop(fs.read(\"/etc/hostname\"))\n", NULL, out, err, sizeof out) == 1);
    CHECK(strstr(err, "sandbox") != NULL);

    /* Sleeping is allowed, up to the limit, and tasks take turns. */
    CHECK(run("seed time\ntime.sleep(0.01)\ndrop(\"woke\")\n", NULL, out, err, sizeof out) == 0);
    CHECK_STR(out, "woke\n");
    PlumPlaygroundLimits nap = {0, 0, 0.05};
    CHECK(run("seed time\ntime.sleep(0.01)\ntime.sleep(1)\ndrop(\"no\")\n", &nap, out, err,
              sizeof out) == 1);
    CHECK_STR(out, "");
    CHECK(strstr(err, "stopped sleeping more than 0.05 seconds") != NULL);
    PlumPlaygroundLimits none = {0, 0, -1};
    CHECK(run("seed time\ntime.sleep(0.01)\n", &none, out, err, sizeof out) == 1);
    CHECK(strstr(err, "sandbox") != NULL);
}

static void test_check(void) {
    char *json = plum_playground_check("drop(1)\n", 8);
    CHECK_STR(json, "[]");
    plum_free(json);

    const char *src = "plant x: int = \"a\"\n";
    json = plum_playground_check(src, strlen(src));
    CHECK(strstr(json, "\"severity\":\"error\"") != NULL);
    CHECK(strstr(json, "\"line\":1,\"column\":16,\"endLine\":1,\"endColumn\":19") != NULL);
    plum_free(json);

    /* No other files to import. */
    src = "seed ./other\n";
    json = plum_playground_check(src, strlen(src));
    CHECK(strstr(json, "\"severity\":\"error\"") != NULL);
    plum_free(json);
}

static void test_format(void) {
    const char *src = "plant   x=1+2\ndrop( x )\n";
    char *json = plum_playground_format(src, strlen(src));
    CHECK_STR(json, "{\"ok\":true,\"text\":\"plant x = 1 + 2\\ndrop(x)\\n\"}");
    plum_free(json);

    src = "plant = \n";
    json = plum_playground_format(src, strlen(src));
    CHECK(strncmp(json, "{\"ok\":false,\"diagnostics\":[{\"severity\":\"error\"", 46) == 0);
    plum_free(json);
}

void plum_test_playground(void) {
    test_run();
    test_check();
    test_format();
}
