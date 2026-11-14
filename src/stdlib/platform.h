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
#include <stdint.h>

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

/* Waiting on sockets: is each ready to read (or write)? */
typedef struct PlumPollItem {
    intptr_t fd;
    bool write;
    bool ready; /* set by plum_os_poll, also on an error or hang-up */
} PlumPollItem;

/* Wait until one is ready or `timeout_ms` passes (-1: no limit). Returns
 * how many are ready, or -1 on an error. */
int plum_os_poll(PlumPollItem *items, size_t count, int timeout_ms);

/*
 * A poller: sockets armed to wake their waiter once, when ready. epoll on
 * Linux, kqueue on macOS and the BSDs, poll (WSAPoll on Windows) over the
 * armed sockets elsewhere. `who` is handed back when the socket is ready.
 */
typedef struct PlumPoller PlumPoller;

PlumPoller *plum_poller_new(void);
void plum_poller_free(PlumPoller *p);
/* Wake `who` once when `fd` can be read (or written). */
void plum_poller_arm(PlumPoller *p, intptr_t fd, bool write, void *who);
/* Stop waiting for `fd` (its waiter was woken some other way). */
void plum_poller_disarm(PlumPoller *p, intptr_t fd, bool write);
/* How many sockets are armed. */
size_t plum_poller_armed(const PlumPoller *p);
/* Wait up to `timeout_ms` (-1: no limit); the waiters of ready sockets go
 * into `ready` (at most `max`). Returns how many. */
int plum_poller_wait(PlumPoller *p, int timeout_ms, void **ready, int max);

/* "linux", "macos", "windows", or "unix". */
const char *plum_os_name(void);

#endif /* PLUM_STDLIB_PLATFORM_H */
