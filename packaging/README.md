# Package-manager manifests

The templates here are filled with a release's version and checksums by
`tools/release/packaging.sh`:

```sh
gh release download vX.Y.Z -R plumlang/plumlang -p SHA256SUMS
sh tools/release/packaging.sh X.Y.Z SHA256SUMS out
```

The release workflow runs it for every tag and attaches the result to the
run as the `packaging` artifact; the publish job's summary shows the
checksums and the AUR files. The licence each manifest declares comes from
the version: 2.1.4 and earlier were released under the Plumlang Commercial
License only, later releases are `Apache-2.0 OR
LicenseRef-Plumlang-Commercial-1.0`.

| Directory | Package manager | How it is published | Updates |
|---|---|---|---|
| `homebrew/` | Homebrew tap `plumlang/tap` | The release workflow's `homebrew` job | Each release |
| `winget/` | winget `Elixpo.Plumlang` | The release workflow's `winget` job | Each release |
| `npm/` | npm `@plumlang/plumlang` | `.github/workflows/npm.yml` | Each release |
| `aur/` | AUR `plum-bin` | Pushed by hand (below) | By hand, each release |
| `scoop/` | Scoop | The Extras bucket, once it accepts Plumlang (below) | Scoop's `checkver` / `autoupdate` |
| `aqua/` | aqua and mise (`aqua:plumlang/plumlang`) | In aquaproj/aqua-registry (merged, #65136) | aqua-registry's Renovate |
| `nix/` | nixpkgs `plum` | NixOS/nixpkgs#592489 (in review) | nixpkgs' update bot |

## AUR: `plum-bin`

The AUR takes any licence, so `plum-bin` can be published now. It installs
the static Linux binary from the release (x86_64, aarch64, riscv64), with
its licence files in `/usr/share/licenses/plum-bin/`. The account that owns
the package needs an SSH key registered on <https://aur.archlinux.org>.

First upload, and each release after it:

```sh
gh release download vX.Y.Z -R plumlang/plumlang -p SHA256SUMS -D out
sh tools/release/packaging.sh X.Y.Z out/SHA256SUMS out
git clone ssh://aur@aur.archlinux.org/plum-bin.git aur-plum-bin   # empty the first time
cp out/aur/PKGBUILD out/aur/.SRCINFO aur-plum-bin/
cd aur-plum-bin
git add PKGBUILD .SRCINFO
git commit -m "Update to X.Y.Z"    # the first time: "Initial upload: plum-bin 2.1.4"
git push origin master
```

On Arch, `makepkg -si` in that folder builds and installs it first, and
`makepkg --printsrcinfo` must print the same `.SRCINFO`.

## Scoop

A filled `scoop/plum.json` installs without a bucket:
`scoop install .\plum.json`.

[ScoopInstaller/Extras](https://github.com/ScoopInstaller/Extras) accepts
proprietary software, but asks for a package-request issue first and for
"at least 100 stars and/or 50 forks". Submit when the repository has them,
from a 2.2.0 or later manifest: open the request issue, wait for a
maintainer to accept it, then a pull request titled `plum: Add version
X.Y.Z` with `bucket/plum.json`, and comment `/verify` on it. Version bumps
are then made by Scoop's Excavator from `checkver` and `autoupdate`; it
reads the hashes from the release's `SHA256SUMS`.

## aqua and mise

`aqua/registry.yaml` and `aqua/pkg.yaml` are the files for
`pkgs/plumlang/plumlang/` in
[aquaproj/aqua-registry](https://github.com/aquaproj/aqua-registry),
scaffolded with `argd s plumlang/plumlang` as its guide requires. They are
in the registry (aquaproj/aqua-registry#65136):

```sh
aqua g -i plumlang/plumlang      # aqua
mise use aqua:plumlang/plumlang  # mise
```

aqua-registry's Renovate picks up new releases by itself. mise can also
install from the GitHub releases directly: `mise use
github:plumlang/plumlang`. A short name in mise's own registry (`mise use
plum`) is only given to tools with thousands of stars, and is requested in
a mise Discussion, not a pull request.

## nixpkgs

`nix/package.nix` builds `plum` from source. It is the package submitted to
nixpkgs as `pkgs/by-name/pl/plum/package.nix` in NixOS/nixpkgs#592489 (in
review), except that it leaves `maintainers` empty. Until it is merged,
build it from a clone:

```sh
nix-build -E 'with import <nixpkgs> {}; callPackage ./packaging/nix/package.nix {}'
./result/bin/plum version
```

Once it is in nixpkgs: `nix profile install nixpkgs#plum`, or `plum` in
`environment.systemPackages` on NixOS. Keep this file in step with the
nixpkgs copy when reviewers ask for changes. nixpkgs' contribution policy
requires disclosing automated help in commits (an `Assisted-by:` trailer)
and keeping pull request text and replies the author's own.
