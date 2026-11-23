# LICENSES - directory layout

Plumlang is licensed under the **Apache License 2.0**, or, as an
alternative, the **Plumlang Commercial License 1.0**:
`Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0`. The top-level
[`LICENSE`](../LICENSE) is the Apache-2.0 text, and anyone may use Plumlang
under it. The Plumlang Commercial License is for those who hold a written
commercial agreement with Elixpo (hello@elixpo.com), for example for
warranty, support, indemnity or trademark terms that Apache-2.0 does not
give. The structure of this directory mirrors the Linux kernel's
`LICENSES/` tree.

Rules for the Elixpo and Plumlang names and logos live in
[`TRADEMARKS.md`](../TRADEMARKS.md).

## Subdirectories

```
preferred/    Licence texts used in this repository.
              Everything is Apache-2.0 OR
              LicenseRef-Plumlang-Commercial-1.0, except the third-party
              code under vendor/, which keeps its own licence.

exceptions/   Exceptions to the preferred licences (none yet).

notices/      Third-party attributions (the upstream credit list).
```

## Index

| Path | SPDX ID | Used for |
|---|---|---|
| `../LICENSE` / `preferred/Apache-2.0` | `Apache-2.0` | All source code, build files, scripts, tests, documentation, examples and brand assets (the primary licence), and third-party code under `vendor/`: Mbed TLS (`vendor/mbedtls`). |
| `preferred/LicenseRef-Plumlang-Commercial-1.0` | `LicenseRef-Plumlang-Commercial-1.0` | The alternative to Apache-2.0 for the same files, for holders of a written commercial agreement with Elixpo. Not used by `vendor/`. |

Third-party code vendored under `vendor/` keeps its own licence, and its text
is added to `preferred/` before it is merged (see `PREAMBLE.md` rule 1.3).

## The notice board

Per-product notes (published package names, release channels, editor
extensions, hosted services) are recorded in the [`NOTICE`](NOTICE) file.
Third-party credits are in [`notices/notice-board`](notices/notice-board).

## Contributions

A contribution is submitted under Apache-2.0, and by submitting a pull
request you also grant Elixpo the right to relicense it, as `PREAMBLE.md`
rule 1.4 describes; you confirm you have the right to submit it. The
[Developer Certificate of Origin](https://developercertificate.org) applies to
every commit. There is no separate CLA.
