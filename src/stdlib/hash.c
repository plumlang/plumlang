/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/* The hash module: cryptographic digests of text, as lowercase hex.
 * SHA-256 comes from the vendored Mbed TLS, through its PSA crypto API. */
#include "stdlib/native.h"

#include "psa/crypto.h"

#define ARGS PlumNative *n, const PlumValue *args, int argc, PlumValue *result

/* sha256(text): the digest of the text's bytes as 64 lowercase hex digits. */
static bool hash_sha256(ARGS) {
    (void)argc;
    const PlumString *text = plum_as_string(args[0]);
    psa_status_t status = psa_crypto_init(); /* once per process; later calls do nothing */
    uint8_t digest[32];
    size_t got = 0;
    if (status == PSA_SUCCESS) {
        status = psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)text->chars, text->len, digest,
                                  sizeof digest, &got);
    }
    if (status != PSA_SUCCESS || got != sizeof digest) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "sha256: the crypto library failed (%d)",
                                 (int)status);
    }
    static const char hex[] = "0123456789abcdef";
    char out[64];
    for (size_t i = 0; i < sizeof digest; i++) {
        out[2 * i] = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 0x0F];
    }
    *result = plum_str_value(out, sizeof out);
    return true;
}

static const PlumNativeFunction functions[] = {
    {"sha256", "(text: str) -> str", hash_sha256,
     "the SHA-256 digest of the text's bytes, as 64 lowercase hex digits"},
};

const PlumNativeModule plum_module_hash = {
    "hash", "cryptographic digests", functions, sizeof functions / sizeof functions[0], NULL, 0,
};
