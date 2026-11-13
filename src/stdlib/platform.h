/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The few things the standard library needs from the operating system
 * beyond standard C: sleeping, a steady clock, directories. One file
 * holds the POSIX and the Windows versions.
 */
#ifndef PLUM_STDLIB_PLATFORM_H
#define PLUM_STDLIB_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

/* Seconds since 1970-01-01 00:00 UTC, with a fraction. */
double plum_os_now(void);

/* Seconds on a clock that only goes forward (not the time of day). */
double plum_os_monotonic(void);

void plum_os_sleep(double seconds);

bool plum_os_is_dir(const char *path);

/* 0, or an errno-style code for strerror. */
int plum_os_make_dir(const char *path);
int plum_os_remove_dir(const char *path);

/*
 * The names in a directory (without "." and ".."), each passed to `add`.
 * Returns 0 or an errno-style code.
 */
int plum_os_list_dir(const char *path, void (*add)(void *ctx, const char *name), void *ctx);

/* The working directory into `buf`; false if it doesn't fit. */
bool plum_os_cwd(char *buf, size_t size);

/* A directory for temporary files, into `buf`. */
void plum_os_temp_dir(char *buf, size_t size);

/* "linux", "macos", "windows", or "unix". */
const char *plum_os_name(void);

#endif /* PLUM_STDLIB_PLATFORM_H */
