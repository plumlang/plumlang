/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/* The os module: the program's arguments, the environment, running
 * commands. */
#include "stdlib/native.h"

#include "stdlib/platform.h"

#include <stdlib.h>
#include <string.h>

#define ARGS PlumNative *n, const PlumValue *args, int argc, PlumValue *result

static bool os_args(ARGS) {
    (void)args;
    (void)argc;
    int count = plum_native_argc(n);
    /* the first one is the program's file; its own arguments follow */
    PlumList *out = plum_list_new(count > 1 ? (size_t)count - 1 : 0);
    for (int i = 1; i < count; i++) {
        const char *a = plum_native_arg(n, i);
        plum_list_push(out, plum_str_value(a, strlen(a)));
    }
    *result = plum_obj(&out->obj);
    return true;
}

static bool os_env(ARGS) {
    (void)n;
    (void)argc;
    const char *v = getenv(plum_as_string(args[0])->chars);
    *result = v != NULL ? plum_str_value(v, strlen(v)) : plum_none();
    return true;
}

static bool os_platform(ARGS) {
    (void)n;
    (void)args;
    (void)argc;
    const char *name = plum_os_name();
    *result = plum_str_value(name, strlen(name));
    return true;
}

static bool os_cwd(ARGS) {
    (void)args;
    (void)argc;
    char buf[4096];
    if (!plum_os_cwd(buf, sizeof buf)) {
        return plum_native_throw(n, PLUM_ERROR_IO, "cwd(): the working directory can't be read");
    }
    *result = plum_str_value(buf, strlen(buf));
    return true;
}

/* run(command): through the system's shell; its exit code. */
static bool os_run(ARGS) {
    (void)argc;
    const PlumString *cmd = plum_as_string(args[0]);
    if (memchr(cmd->chars, '\0', cmd->len) != NULL) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "run(): the command contains a zero byte");
    }
    fflush(plum_native_out(n)); /* the command's output comes after ours */
    fflush(plum_native_err(n));
    int status = system(cmd->chars);
    if (status == -1) {
        return plum_native_throw(n, PLUM_ERROR_IO, "run(): the command couldn't be started");
    }
#if !defined(_WIN32)
    /* the shell's exit status, as the shell reports it */
    if ((status & 0x7F) == 0) {
        status = (status >> 8) & 0xFF;
    } else {
        status = 128 + (status & 0x7F); /* ended by a signal */
    }
#endif
    *result = plum_int(status);
    return true;
}

static const PlumNativeFunction functions[] = {
    {"args", "() -> list[str]", os_args,
     "the arguments after the program's file: plum run app.plum a b gives [\"a\", \"b\"]"},
    {"env", "(name: str) -> str?", os_env, "an environment variable, or none if it isn't set"},
    {"platform", "() -> str", os_platform, "\"linux\", \"macos\", \"windows\" or \"unix\""},
    {"cwd", "() -> str", os_cwd, "the working directory"},
    {"run", "(command: str) -> int", os_run,
     "run a command through the system's shell and give its exit code"},
};

const PlumNativeModule plum_module_os = {
    "os",      "the program's arguments, the environment and running commands",
    functions, sizeof functions / sizeof functions[0],
    NULL,      0,
};
