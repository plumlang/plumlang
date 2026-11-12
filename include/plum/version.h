/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/* Plumlang version and build information. */
#ifndef PLUM_VERSION_H
#define PLUM_VERSION_H

#define PLUM_VERSION_MAJOR 0
#define PLUM_VERSION_MINOR 0
#define PLUM_VERSION_PATCH 1

#define PLUM_STR_(x) #x
#define PLUM_STR(x)  PLUM_STR_(x)

#define PLUM_VERSION                                                                               \
    PLUM_STR(PLUM_VERSION_MAJOR) "." PLUM_STR(PLUM_VERSION_MINOR) "." PLUM_STR(PLUM_VERSION_PATCH)

/* The build passes the short commit hash with -DPLUM_COMMIT="abc1234". */
#ifndef PLUM_COMMIT
#define PLUM_COMMIT "unknown"
#endif

#if defined(_WIN32)
#define PLUM_OS "windows"
#elif defined(__APPLE__)
#define PLUM_OS "macos"
#elif defined(__linux__)
#define PLUM_OS "linux"
#elif defined(__FreeBSD__)
#define PLUM_OS "freebsd"
#elif defined(__unix__)
#define PLUM_OS "unix"
#else
#define PLUM_OS "unknown-os"
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define PLUM_ARCH "x86_64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define PLUM_ARCH "arm64"
#elif defined(__i386__) || defined(_M_IX86)
#define PLUM_ARCH "x86"
#elif defined(__arm__) || defined(_M_ARM)
#define PLUM_ARCH "arm"
#elif defined(__riscv) && __riscv_xlen == 64
#define PLUM_ARCH "riscv64"
#elif defined(__riscv)
#define PLUM_ARCH "riscv32"
#else
#define PLUM_ARCH "unknown-arch"
#endif

#if defined(__clang__)
#define PLUM_CC "clang " __clang_version__
#elif defined(__GNUC__)
#define PLUM_CC "gcc " __VERSION__
#elif defined(_MSC_VER)
#define PLUM_CC "msvc " PLUM_STR(_MSC_VER)
#else
#define PLUM_CC "unknown compiler"
#endif

#endif /* PLUM_VERSION_H */
