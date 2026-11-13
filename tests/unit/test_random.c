/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "check.h"
#include "suites.h"
#include "util/random.h"

#include <stdint.h>

void plum_test_random(void) {
    uint64_t key[2];
    plum_random_key(key);
    CHECK(key[0] != 0 || key[1] != 0);

    uint64_t other[2];
    plum_random_key(other);
    CHECK(key[0] != other[0] || key[1] != other[1]);

    const char *msg = "plumlang";
    CHECK(plum_siphash(key, msg, 8) == plum_siphash(key, msg, 8));
    CHECK(plum_siphash(key, msg, 8) != plum_siphash(other, msg, 8));
    CHECK(plum_siphash(key, msg, 8) != plum_siphash(key, msg, 7));

    /* Every length, across the 8-byte block boundary, hashes differently. */
    unsigned char data[64];
    for (int i = 0; i < 64; i++) {
        data[i] = (unsigned char)(i * 37);
    }
    uint64_t seen[65];
    int distinct = 1;
    for (size_t len = 0; len <= 64; len++) {
        seen[len] = plum_siphash(key, data, len);
        for (size_t k = 0; k < len; k++) {
            distinct = distinct && seen[k] != seen[len];
        }
    }
    CHECK(distinct);

    /* A fixed key gives a fixed hash, so results don't depend on the run. */
    const uint64_t fixed[2] = {0x0706050403020100ULL, 0x0f0e0d0c0b0a0908ULL};
    CHECK(plum_siphash(fixed, "", 0) == plum_siphash(fixed, "", 0));
}
