# Releasing Plumlang

A release is a tag. Pushing `vX.Y.Z` runs
[`.github/workflows/release.yml`](../.github/workflows/release.yml), which
builds every platform, installs each build on a clean machine, and publishes
the GitHub release. Nothing is uploaded by hand.

## Cutting a release

1. Set the version in `include/plum/version.h` (`PLUM_VERSION_MAJOR`,
   `_MINOR`, `_PATCH`) and in `project(plumlang VERSION …)` in
   `CMakeLists.txt`. `sh tools/release/version.sh` checks they agree.
2. In `CHANGELOG.md`, rename `## Unreleased` to `## vX.Y.Z — YYYY-MM-DD` and
   start a new, empty `## Unreleased` above it. The section becomes the
   release notes (`sh tools/release/notes.sh X.Y.Z` prints them).
3. Merge that as a pull request.
4. Tag `main` and push the tag:

   ```sh
   git tag -s vX.Y.Z -m "Plumlang X.Y.Z"
   git push origin vX.Y.Z
   ```

The workflow stops before publishing if the tag and the source disagree, if
`CHANGELOG.md` has no section for the version, or if any build, test or
install check fails.

Pull requests that change the release files run the same workflow without
publishing, and so does **Actions › Release › Run workflow**.

## What a release contains

Every build is optimised for speed beyond plain `make`: link-time
optimisation everywhere, and profile-guided optimisation where the build
machine can run the training (`make release-pgo`; the gain is in
[bench/README.md](../bench/README.md)).

| File | Built on | Notes |
|---|---|---|
| `plum-X.Y.Z-linux-x86_64.tar.gz` | Alpine (musl), x86_64 | Static: runs on any Linux. LTO + PGO. Full test suite. |
| `plum-X.Y.Z-linux-arm64.tar.gz` | Alpine (musl), arm64 | Static. LTO + PGO. Full test suite. |
| `plum-X.Y.Z-linux-riscv64.tar.gz` | Alpine (musl), riscv64 under QEMU | Static. LTO. Examples only (emulation is slow). |
| `plum-X.Y.Z-macos-universal.tar.gz` | macOS, arm64 + x86_64 | macOS 11 or later. arm64: LTO + PGO, full test suite. x86_64 (cross-compiled): LTO. |
| `plum-X.Y.Z-windows-x86_64.zip` | MSVC, x64 | Static C runtime. LTCG. Full test suite. |
| `plum-X.Y.Z-windows-arm64.zip` | MSVC, ARM64 (cross-compiled) | Static C runtime. LTCG. |
| `plum-X.Y.Z-wasm.tar.gz` | Emscripten (`make wasm`) | `plum.js` and `plum.wasm` for the browser ([playground.md](playground.md)). Checked against the native build under Node. |
| `install.sh`, `install.ps1` | | The installers. |
| `SHA256SUMS` | | Checksums of every file above. |

Each archive holds one folder with `plum` (or `plum.exe`; for wasm,
`plum.js`, `plum.wasm` and `playground.md`), `LICENSE`, `LICENSES/`,
`README.md` and `CHANGELOG.md`.

Every file has a [build attestation](https://docs.github.com/actions/security-for-github-actions/using-artifact-attestations),
which proves it was built by this workflow from the tagged commit:

```sh
gh attestation verify plum-X.Y.Z-linux-x86_64.tar.gz --repo plumlang/plumlang
```

## Installing

```sh
curl -fsSL https://plum.elixpo.com/install.sh | sh
```

```powershell
irm https://plum.elixpo.com/install.ps1 | iex
```

The installers pick the build for the machine, check it against
`SHA256SUMS`, and install into `~/.plum/bin` (Windows:
`%LOCALAPPDATA%\Plumlang\bin`) without administrator rights.
`PLUM_VERSION` picks a version and `PLUM_INSTALL` the folder. Before
publishing, the workflow runs both installers against the new builds on
Linux x86_64, Linux arm64, macOS and Windows.

## Signing and package managers

These steps run when their secrets are set on the repository (or the
organisation), and print a notice and skip when they are not.

| Step | Secrets | One-time setup |
|---|---|---|
| macOS signing and notarization | `APPLE_CERTIFICATE` (base64 of a Developer ID Application `.p12`), `APPLE_CERTIFICATE_PASSWORD`, `APPLE_SIGNING_IDENTITY` (e.g. `Developer ID Application: Elixpo (TEAMID)`), `APPLE_ID`, `APPLE_TEAM_ID`, `APPLE_APP_PASSWORD` (an app-specific password) | An Apple Developer Program membership. |
| Windows signing | `WINDOWS_CERTIFICATE` (base64 of a code-signing `.pfx`), `WINDOWS_CERTIFICATE_PASSWORD` | A code-signing certificate (OV or EV). |
| Homebrew | `HOMEBREW_TAP_TOKEN`: a fine-grained token with contents write on `plumlang/homebrew-tap` | Create the `plumlang/homebrew-tap` repository. Users then run `brew install plumlang/tap/plum`. |
| npm | `NPM_TOKEN`: an npm automation token that can publish to the `@plumlang` scope | Create the `plumlang` organisation on npm. The `npm` job (`.github/workflows/npm.yml`) publishes the release's `plum-X.Y.Z-wasm.tar.gz` as [`@plumlang/plumlang`](https://www.npmjs.com/package/@plumlang/plumlang), with provenance. To publish a release again, or one tagged before the secret was set: Actions > npm > Run workflow, with the tag. |
| winget | `WINGET_TOKEN`: a classic token with `public_repo`, for the account that opens pull requests on `microsoft/winget-pkgs` | None; the first submission is reviewed by the winget maintainers. Users then run `winget install Elixpo.Plumlang`. |

The Homebrew formula, the winget manifests, the AUR `plum-bin` package and
the Scoop manifest are generated from the templates in
[`packaging/`](../packaging/) by `tools/release/packaging.sh`, and attached
to the workflow run as the `packaging` artifact even when the secrets are
missing, so they can be submitted by hand. The AUR has no token: push its
`PKGBUILD` and `.SRCINFO`, which the publish job's summary also shows, as
[`packaging/README.md`](../packaging/README.md) describes, with Scoop,
aqua, mise and nixpkgs.
