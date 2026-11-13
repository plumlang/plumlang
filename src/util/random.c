/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#if defined(_WIN32)
#define _CRT_RAND_S /* rand_s, from the OS's cryptographic generator */
#endif

#include "util/random.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void plum_random_key(uint64_t key[2]) {
    int ok = 0;
#if defined(_WIN32)
    unsigned int parts[4];
    ok = 1;
    for (int i = 0; i < 4; i++) {
        if (rand_s(&parts[i]) != 0) {
            ok = 0;
        }
    }
    if (ok) {
        key[0] = ((uint64_t)parts[0] << 32) | parts[1];
        key[1] = ((uint64_t)parts[2] << 32) | parts[3];
    }
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f != NULL) {
        ok = fread(key, sizeof(uint64_t), 2, f) == 2;
        fclose(f);
    }
#endif
    if (!ok) {
        /* No OS randomness: still different per run, if guessable. */
        uintptr_t here = (uintptr_t)&ok;
        key[0] = (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ULL ^ (uint64_t)here;
        key[1] = (uint64_t)clock() * 0xC2B2AE3D27D4EB4FULL ^ (uint64_t)(here >> 4);
    }
}

#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))

#define SIPROUND           \
    do {                   \
        v0 += v1;          \
        v1 = ROTL(v1, 13); \
        v1 ^= v0;          \
        v0 = ROTL(v0, 32); \
        v2 += v3;          \
        v3 = ROTL(v3, 16); \
        v3 ^= v2;          \
        v0 += v3;          \
        v3 = ROTL(v3, 21); \
        v3 ^= v0;          \
        v2 += v1;          \
        v1 = ROTL(v1, 17); \
        v1 ^= v2;          \
        v2 = ROTL(v2, 32); \
    } while (0)

uint64_t plum_siphash(const uint64_t key[2], const void *data, size_t len) {
    const unsigned char *in = data;
    uint64_t v0 = 0x736f6d6570736575ULL ^ key[0];
    uint64_t v1 = 0x646f72616e646f6dULL ^ key[1];
    uint64_t v2 = 0x6c7967656e657261ULL ^ key[0];
    uint64_t v3 = 0x7465646279746573ULL ^ key[1];
    uint64_t b = (uint64_t)len << 56;

    size_t full = len - (len % 8);
    for (size_t i = 0; i < full; i += 8) {
        uint64_t m = 0;
        for (int k = 0; k < 8; k++) {
            m |= (uint64_t)in[i + (size_t)k] << (8 * k); /* little-endian, on any machine */
        }
        v3 ^= m;
        SIPROUND;
        v0 ^= m;
    }
    for (size_t k = 0; k < len % 8; k++) {
        b |= (uint64_t)in[full + k] << (8 * k);
    }
    v3 ^= b;
    SIPROUND;
    v0 ^= b;
    v2 ^= 0xff;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}
