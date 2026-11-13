/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "stdlib/platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>

double plum_os_now(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft); /* 100 ns steps since 1601 */
    ULARGE_INTEGER t;
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    return (double)(t.QuadPart - 116444736000000000ULL) / 1e7;
}

double plum_os_monotonic(void) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

void plum_os_sleep(double seconds) {
    if (seconds > 0) {
        Sleep((DWORD)(seconds * 1000.0 + 0.5));
    }
}

bool plum_os_is_dir(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

int plum_os_make_dir(const char *path) {
    return _mkdir(path) == 0 ? 0 : errno;
}

int plum_os_remove_dir(const char *path) {
    return _rmdir(path) == 0 ? 0 : errno;
}

int plum_os_list_dir(const char *path, void (*add)(void *ctx, const char *name), void *ctx) {
    char pattern[MAX_PATH + 3];
    if (snprintf(pattern, sizeof pattern, "%s\\*", path) >= (int)sizeof pattern) {
        return ENAMETOOLONG;
    }
    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA(pattern, &found);
    if (h == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_PATH_NOT_FOUND || GetLastError() == ERROR_FILE_NOT_FOUND
                   ? ENOENT
                   : EACCES;
    }
    do {
        if (strcmp(found.cFileName, ".") != 0 && strcmp(found.cFileName, "..") != 0) {
            add(ctx, found.cFileName);
        }
    } while (FindNextFileA(h, &found));
    FindClose(h);
    return 0;
}

bool plum_os_cwd(char *buf, size_t size) {
    return _getcwd(buf, (int)size) != NULL;
}

void plum_os_temp_dir(char *buf, size_t size) {
    DWORD n = GetTempPathA((DWORD)size, buf);
    if (n == 0 || n >= size) {
        snprintf(buf, size, ".");
        return;
    }
    if (n > 1 && (buf[n - 1] == '\\' || buf[n - 1] == '/')) {
        buf[n - 1] = '\0'; /* no trailing separator, as on other systems */
    }
}

const char *plum_os_name(void) {
    return "windows";
}

#else
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

double plum_os_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

double plum_os_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void plum_os_sleep(double seconds) {
    if (seconds <= 0) {
        return;
    }
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
        /* interrupted by a signal: sleep the rest */
    }
}

bool plum_os_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int plum_os_make_dir(const char *path) {
    return mkdir(path, 0777) == 0 ? 0 : errno;
}

int plum_os_remove_dir(const char *path) {
    return rmdir(path) == 0 ? 0 : errno;
}

int plum_os_list_dir(const char *path, void (*add)(void *ctx, const char *name), void *ctx) {
    DIR *d = opendir(path);
    if (d == NULL) {
        return errno;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
            add(ctx, e->d_name);
        }
    }
    closedir(d);
    return 0;
}

bool plum_os_cwd(char *buf, size_t size) {
    return getcwd(buf, size) != NULL;
}

void plum_os_temp_dir(char *buf, size_t size) {
    const char *t = getenv("TMPDIR");
    snprintf(buf, size, "%s", t != NULL && t[0] != '\0' ? t : "/tmp");
    size_t n = strlen(buf);
    if (n > 1 && buf[n - 1] == '/') {
        buf[n - 1] = '\0';
    }
}

const char *plum_os_name(void) {
#if defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unix";
#endif
}
#endif
