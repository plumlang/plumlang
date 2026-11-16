/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/* The hash module: cryptographic digests of text, as lowercase hex.
 * SHA-256 comes from the vendored Mbed TLS, through its PSA crypto API. */
#if defined(__EMSCRIPTEN__)
#define _DEFAULT_SOURCE /* getentropy */
#endif

#include "stdlib/native.h"

#include "psa/crypto.h"

#if defined(__EMSCRIPTEN__)
#include "mbedtls/platform.h"

#include <unistd.h>

/* The browser has no /dev/urandom for Mbed TLS's own entropy source
 * (src/stdlib/tls/crypto_user_config.h): crypto.getRandomValues instead,
 * by way of getentropy, which gives at most 256 bytes a call. */
int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags, size_t *estimate_bits,
                                 unsigned char *output, size_t output_size) {
    if (flags != 0) {
        return PSA_ERROR_NOT_SUPPORTED;
    }
    for (size_t done = 0; done < output_size;) {
        size_t n = output_size - done < 256 ? output_size - done : 256;
        if (getentropy(output + done, n) != 0) {
            return PSA_ERROR_INSUFFICIENT_ENTROPY;
        }
        done += n;
    }
    *estimate_bits = 8 * output_size;
    return 0;
}
#endif

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
