/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Randomness from the operating system, and SipHash-1-3, a keyed hash.
 *
 * Maps hash their keys with SipHash under a random key, so a remote peer
 * sending map keys (HTTP headers, JSON fields) can't choose ones that all
 * collide and make every lookup slow.
 */
#ifndef PLUM_UTIL_RANDOM_H
#define PLUM_UTIL_RANDOM_H

#include <stddef.h>
#include <stdint.h>

/* Fill `key` with random bits from the operating system. */
void plum_random_key(uint64_t key[2]);

/* SipHash-1-3 of data[0..len) under `key`. */
uint64_t plum_siphash(const uint64_t key[2], const void *data, size_t len);

#endif /* PLUM_UTIL_RANDOM_H */
