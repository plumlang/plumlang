# Mbed TLS, vendored

This directory is third-party code: **Mbed TLS 4.1.1** (the 4.1 long-term
support line, supported until March 2029), with the **TF-PSA-Crypto 1.1.1**
it ships. `plum` uses it for TLS (`net.tls_connect`, `net.listen_tls`,
`https://` in `net.http`). It is built into the `plum` binary; nothing is
linked at run time.

- Upstream: <https://github.com/Mbed-TLS/mbedtls>
- Release: <https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-4.1.1>
- Archive: `mbedtls-4.1.1.tar.bz2`, SHA-256
  `3359a349e23db3d5536fcee032ae7b2ecbfc08972fab643089b5cbf2a375c98c`
  (checked against `mbedtls-4.1.1-sha256sum.txt` from the same release)
- Licence: Apache-2.0 (upstream offers Apache-2.0 OR GPL-2.0-or-later;
  Plumlang takes it under Apache-2.0 only). See [`LICENSE`](LICENSE),
  `LICENSES/preferred/Apache-2.0` and `LICENSES/notices/notice-board`
  (PREAMBLE rule 1.3). The files keep their upstream copyright and
  licence lines.

## What is here

Only what the build needs, unchanged from the release:

| Here | From the release |
|---|---|
| `LICENSE` | `LICENSE` |
| `include/` | `include/` (Mbed TLS headers) |
| `library/` | `library/*.c`, `library/*.h` (TLS and X.509) |
| `tf-psa-crypto/LICENSE` | `tf-psa-crypto/LICENSE` |
| `tf-psa-crypto/include/` | `tf-psa-crypto/include/` |
| `tf-psa-crypto/{core,dispatch,extras,platform,utilities}/` | the same directories (`*.c`, `*.h`) |
| `tf-psa-crypto/drivers/builtin/` | `tf-psa-crypto/drivers/builtin/` |

Left out: tests, programs, docs, scripts, the `framework/` submodule, the
CMake and make files, and the optional crypto drivers (`everest`,
`p256-m`, `pqcp`), which the configuration doesn't use. The release
ships its generated sources (`error.c`, `version_features.c`,
`ssl_debug_helpers_generated.c`, `psa_crypto_driver_wrappers*.{c,h}`,
the `*_config_check_*.h` files), so the build needs no CMake, Python or
Perl.

## How it is built

Every `.c` file above is compiled by the repository's `Makefile` and
`CMakeLists.txt` with the upstream code's own, relaxed warnings (our code
keeps `-Werror`), each function in a section of its own so the linker
drops what plum never calls, and kept out of `make format`. The
configuration is the upstream default, trimmed by two files of ours that
are read after it:

- `src/stdlib/tls/mbedtls_user_config.h` (`MBEDTLS_USER_CONFIG_FILE`):
  no DTLS, no TLS 1.2 pre-shared keys, no server session cache or tickets,
  no renegotiation, X.509 certificates read only (no CSRs, CRLs or
  writing), no `net_sockets.c` or `timing.c` (plum has its own).
- `src/stdlib/tls/crypto_user_config.h` (`TF_PSA_CRYPTO_USER_CONFIG_FILE`):
  no key store on disk, no self-tests, and only the algorithms the usual
  TLS 1.2/1.3 cipher suites need (AES-GCM/CCM/CBC, ChaCha20-Poly1305,
  SHA-1/2, ECDHE/ECDSA on P-256/384/521 and X25519/X448, RSA).

On Windows the library needs `bcrypt` (random numbers), and plum's TLS
code `crypt32` (the system's trusted certificates).

## Updating

1. Download the new release's `.tar.bz2` and its `-sha256sum.txt`, check
   the sum, and extract it into an empty directory outside the repository.
2. Check that it still ships the generated files listed above. If it
   doesn't, they have to be generated with upstream's scripts first.
3. Replace `LICENSE`, `include/`, `library/` and the `tf-psa-crypto/`
   directories listed above with the new ones; delete the
   `CMakeLists.txt`, `Makefile`, `*.make` and `.gitignore` files that come
   with them.
4. Update the version and checksum here and in
   `LICENSES/notices/notice-board`.
5. Build (`make`, `make sanitize test`, and the CMake build): a new
   release may rename options in the two configuration files, or need
   source files in a new directory.
