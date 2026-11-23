#!/bin/sh
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
#
# Install Plumlang on Linux or macOS:
#
#   curl -fsSL https://plum.elixpo.com/install.sh | sh
#
# Settings, as environment variables:
#   PLUM_VERSION   the version to install, e.g. 1.2.3 (default: the newest release)
#   PLUM_INSTALL   where to install (default: ~/.plum); the binary goes in bin/
#
# The archive is checked against the release's SHA256SUMS before anything is
# installed. Nothing outside PLUM_INSTALL is changed.
set -eu

REPO="plumlang/plumlang"
BASE="${PLUM_DOWNLOAD_BASE:-https://github.com/$REPO/releases/download}"
API="${PLUM_API:-https://api.github.com/repos/$REPO/releases?per_page=1}"
DEST="${PLUM_INSTALL:-$HOME/.plum}"

say() { printf '%s\n' "$*"; }
fail() { printf 'plum install: %s\n' "$*" >&2; exit 1; }

fetch() { # fetch <url> <file>
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --proto '=https' --tlsv1.2 "$1" -o "$2" 2>/dev/null || curl -fsSL "$1" -o "$2"
    elif command -v wget >/dev/null 2>&1; then
        wget -q "$1" -O "$2"
    else
        fail "needs curl or wget"
    fi
}

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d ' ' -f 1
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d ' ' -f 1
    else
        fail "needs sha256sum or shasum to check the download"
    fi
}

case "$(uname -s)" in
    Linux) os=linux ;;
    Darwin) os=macos ;;
    MINGW* | MSYS* | CYGWIN*) fail "on Windows, use install.ps1: irm https://plum.elixpo.com/install.ps1 | iex" ;;
    *) fail "no build for $(uname -s); build from source: https://github.com/$REPO" ;;
esac
if [ "$os" = macos ]; then
    target=macos-universal
else
    case "$(uname -m)" in
        x86_64 | amd64) target=linux-x86_64 ;;
        aarch64 | arm64) target=linux-arm64 ;;
        riscv64) target=linux-riscv64 ;;
        *) fail "no build for linux on $(uname -m); build from source: https://github.com/$REPO" ;;
    esac
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

version="${PLUM_VERSION:-}"
version="${version#v}"
if [ -z "$version" ] || [ "$version" = latest ]; then
    fetch "$API" "$tmp/releases.json" || fail "could not reach GitHub to find the newest release"
    version="$(sed -n 's/.*"tag_name": *"v\{0,1\}\([^"]*\)".*/\1/p' "$tmp/releases.json" | head -n 1)"
    [ -n "$version" ] || fail "no release found; set PLUM_VERSION"
fi

archive="plum-$version-$target.tar.gz"
say "Installing Plumlang $version ($target) into $DEST"
fetch "$BASE/v$version/$archive" "$tmp/$archive" || fail "could not download $archive"
fetch "$BASE/v$version/SHA256SUMS" "$tmp/SHA256SUMS" || fail "could not download SHA256SUMS"

want="$(awk -v f="$archive" '$2 == f || $2 == "*" f { print $1 }' "$tmp/SHA256SUMS")"
[ -n "$want" ] || fail "SHA256SUMS has no entry for $archive"
[ "$(sha256 "$tmp/$archive")" = "$want" ] || fail "checksum mismatch for $archive; nothing was installed"

tar -xzf "$tmp/$archive" -C "$tmp"
mkdir -p "$DEST/bin"
cp "$tmp/plum-$version-$target/plum" "$DEST/bin/plum.new"
chmod 755 "$DEST/bin/plum.new"
mv -f "$DEST/bin/plum.new" "$DEST/bin/plum"

"$DEST/bin/plum" version >/dev/null 2>&1 || fail "installed, but $DEST/bin/plum does not run"
say "Installed: $("$DEST/bin/plum" version)"

case ":$PATH:" in
    *":$DEST/bin:"*) ;;
    *)
        say ""
        say "Add plum to your PATH, in ~/.profile, ~/.bashrc or ~/.zshrc:"
        say "    export PATH=\"$DEST/bin:\$PATH\""
        ;;
esac
