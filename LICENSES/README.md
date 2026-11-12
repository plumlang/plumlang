# LICENSES - directory layout

Plumlang is licensed under the **Plumlang Commercial License 1.0**
(`LicenseRef-Plumlang-Commercial-1.0`). The top-level [`LICENSE`](../LICENSE)
is that text. The structure of this directory mirrors the Linux kernel's
`LICENSES/` tree.

Rules for the Elixpo and Plumlang names and logos live in
[`TRADEMARKS.md`](../TRADEMARKS.md).

## Subdirectories

```
preferred/    Licence texts used in this repository.
              Everything is LicenseRef-Plumlang-Commercial-1.0.

exceptions/   Exceptions to the preferred licences (none yet).

notices/      Third-party attributions (the upstream credit list).
```

## Index

| Path | SPDX ID | Used for |
|---|---|---|
| `../LICENSE` / `preferred/LicenseRef-Plumlang-Commercial-1.0` | `LicenseRef-Plumlang-Commercial-1.0` | All source code, build files, scripts, tests, documentation, examples and brand assets. |

Third-party code vendored under `vendor/` keeps its own licence, and its text
is added to `preferred/` before it is merged (see `PREAMBLE.md` rule 1.3).

## The notice board

Per-product notes (published package names, release channels, editor
extensions, hosted services) are recorded in the [`NOTICE`](NOTICE) file.
Third-party credits are in [`notices/notice-board`](notices/notice-board).

## Contributions

By submitting a pull request you grant Elixpo the licence in section 6 of
`LICENSE`, and you confirm you have the right to submit it. The
[Developer Certificate of Origin](https://developercertificate.org) applies to
every commit. There is no separate CLA.
