#!/bin/sh
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
#
# Tests of packages (docs/orchard.md), offline: the packages in
# tests/orchard/packages are packed with `plum package` into an Orchard
# index in a temporary directory, which `plum add` and the rest read
# through PLUM_ORCHARD=file:///... . The cache goes to a temporary
# PLUM_HOME. If python3 is there, the same index is also served over HTTP
# on 127.0.0.1, to test the network path (PLUM_ORCHARD_NO_HTTP=1 skips it).
#
# Usage: sh tests/orchard.sh ./plum

PLUM=${1:-./plum}
case $PLUM in
    /* | [A-Za-z]:*) ;;
    *) PLUM=$(pwd)/$PLUM ;;
esac
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
server=
cleanup() {
    [ -n "$server" ] && kill "$server" 2>/dev/null
    rm -rf "$tmp"
}
trap cleanup EXIT

# Paths as plum.exe sees them on Windows (MSYS2); as they are elsewhere.
native() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi
}

NO_COLOR=1
export NO_COLOR
unset FORCE_COLOR
PLUM_HOME=$(native "$tmp/home")
export PLUM_HOME
registry="$tmp/registry"
PLUM_ORCHARD="file://$(native "$registry" | sed 's|^\([A-Za-z]:\)|/\1|')"
export PLUM_ORCHARD

pass=0
fail=0

# check NAME EXPECTED_EXIT PATTERN STREAM -- ARGS...: run plum in the
# current directory; PATTERN (for grep) must be in std$STREAM.
check() {
    name=$1 want=$2 pattern=$3 stream=$4
    shift 5
    "$PLUM" "$@" >"$tmp/out" 2>"$tmp/err" </dev/null
    got=$?
    tr -d '\r' <"$tmp/out" >"$tmp/out.txt"
    tr -d '\r' <"$tmp/err" >"$tmp/err.txt"
    if [ "$got" -ne "$want" ]; then
        echo "FAIL $name: exit $got, expected $want"
        sed 's/^/    /' "$tmp/out.txt" "$tmp/err.txt" | head -n 20
        fail=$((fail + 1))
        return
    fi
    if ! grep -q -- "$pattern" "$tmp/$stream.txt"; then
        echo "FAIL $name: std$stream does not contain '$pattern'"
        sed 's/^/    /' "$tmp/out.txt" "$tmp/err.txt" | head -n 20
        fail=$((fail + 1))
        return
    fi
    pass=$((pass + 1))
}

# expect NAME COMMAND...: a shell command that must succeed.
expect() {
    name=$1
    shift
    if "$@"; then
        pass=$((pass + 1))
    else
        echo "FAIL $name"
        fail=$((fail + 1))
    fi
}

# ---- An index, made as the registry's publish workflow makes it ----

mkdir -p "$registry/index" "$registry/packages" "$tmp/bundles" "$tmp/entries"

# publish DIR DEPENDENCIES_JSON: package (a copy of) DIR and list it in the
# index. A package with dependencies installs them first, from the index.
publish() {
    rm -rf "$tmp/src"
    mkdir -p "$tmp/src"
    cp -R "$1" "$tmp/src/pkg"
    if ! (cd "$tmp/src/pkg" && "$PLUM" package --out "$(native "$tmp/bundles")") >"$tmp/pack" 2>&1; then
        echo "FAIL packaging $1"
        sed 's/^/    /' "$tmp/pack"
        fail=$((fail + 1))
        return
    fi
    file=$(tr -d '\r' <"$tmp/pack" | sed -n 's|^  .*/\([^/]*\.plumpkg\)$|\1|p')
    sha=$(tr -d '\r' <"$tmp/pack" | sed -n 's/^  sha256 //p')
    base=${file%.plumpkg}
    pname=${base%-*}
    version=${base##*-}
    mkdir -p "$registry/packages/$pname"
    cp "$tmp/bundles/$file" "$registry/packages/$pname/$version.plumpkg"
    printf '{"version": "%s", "sha256": "%s", "dependencies": %s, "plum": ">=0.0", "published": "2026-11-15T00:00:00Z", "yanked": false}\n' \
        "$version" "$sha" "$2" >>"$tmp/entries/$pname"
}

# index NAME: write index/NA/NAME.json from its entries.
index() {
    prefix=$(printf '%s' "$1" | cut -c1-2)
    mkdir -p "$registry/index/$prefix"
    {
        printf '{"name": "%s", "owners": ["plumlang"], "description": "", "repository": "", "keywords": [],\n "versions": [\n' "$1"
        sed '$!s/$/,/' "$tmp/entries/$1"
        printf ']}\n'
    } >"$registry/index/$prefix/$1.json"
}

P="$here/orchard/packages"
publish "$P/hello-orchard-1.0.0" '{}'
publish "$P/hello-orchard-1.1.0" '{}'
publish "$P/colors-1.0.0" '{}'
publish "$P/colors-2.0.0" '{}'
index colors
publish "$P/paint-1.0.0" '{"colors": "^1.0"}'
publish "$P/brush-1.0.0" '{"colors": "^2.0"}'
for n in hello-orchard colors paint brush; do index "$n"; done
cat >"$registry/search.json" <<'EOF'
{"packages": [
 {"name": "hello-orchard", "version": "1.1.0", "description": "A friendly greeting: the example package of Orchard", "keywords": ["example", "greeting"]},
 {"name": "colors", "version": "2.0.0", "description": "Colour names, a test fixture", "keywords": ["color", "test"]}
]}
EOF

# ---- The same source always gives the same bundle ----

h1=$(sed -n '1s/.*"sha256": "\([0-9a-f]*\)".*/\1/p' "$tmp/entries/hello-orchard")
mkdir -p "$tmp/again"
(cd "$P/hello-orchard-1.0.0" && "$PLUM" package --out "$(native "$tmp/again")") >"$tmp/again.txt" 2>&1
expect "packaging is reproducible" grep -q "sha256 $h1" "$tmp/again.txt"
expect "a bundle is JSON with sorted keys" grep -q '^{"files":{"LICENSE":' "$tmp/again/hello-orchard-1.0.0.plumpkg"
expect "a bundle ends with its name and version" grep -q '"format":1,"name":"hello-orchard","version":"1.0.0"}$' "$tmp/again/hello-orchard-1.0.0.plumpkg"

# ---- A project that uses packages ----

app="$tmp/app"
mkdir -p "$app"
cat >"$app/plum.toml" <<'EOF'
[package]
name = "app"
version = "0.1.0"
entry = "main.plum"
EOF
cat >"$app/main.plum" <<'EOF'
seed hello_orchard
seed hello_orchard.text.shout
drop(hello_orchard.greet("Ada"))
drop(shout.loud("quiet"))
EOF
cd "$app" || exit 1

check "an undeclared package"           1 "E0908.*run \`plum add hello-orchard\`" err -- run .
check "a stdlib name is no package"     1 "no standard module 'net.nothing'" err -- check "$(native "$here/orchard/stdlib-name.plum")"
check "add the newest version"          0 "added hello-orchard ^1.1.0 (1.1.0)" out -- add hello-orchard
expect "add writes the range"           grep -q '^hello-orchard = "^1.1.0"$' plum.toml
expect "add writes plum.lock"           grep -q '^\[packages.hello-orchard\]$' plum.lock
expect "the lock has the checksum"      grep -q "^sha256 = \"[0-9a-f]\{64\}\"$" plum.lock
check "run with the package"            0 "^Hi, Ada, from the orchard!$" out -- run .
check "seed a module of a package"      0 "^QUIET$" out -- run .
check "install from the lock"           0 "1 packages from plum.lock, 0 downloaded" out -- install
rm -rf "$tmp/home"
check "install downloads again"         0 "installed hello-orchard 1.1.0" out -- install
check "and it runs"                     0 "^Hi, Ada" out -- run .

# Yanking 1.1.0: a lock that has it still installs it; nothing new picks it.
awk 'NR == 2 { sub(/"yanked": false/, "\"yanked\": true") } { print }' "$tmp/entries/hello-orchard" >"$tmp/y"
mv "$tmp/y" "$tmp/entries/hello-orchard"
index hello-orchard
rm -rf "$tmp/home"
check "a yanked version installs from a lock" 0 "installed hello-orchard 1.1.0" out -- install
check "remove"                          0 "removed hello-orchard" out -- remove hello-orchard
expect "remove takes it out of plum.toml" sh -c '! grep -q hello-orchard plum.toml'
expect "and out of plum.lock"           sh -c '! grep -q hello-orchard plum.lock'
check "a yanked version isn't resolved" 1 "E0922.*1.1.0 would, but it was yanked" err -- add hello-orchard@=1.1.0
check "add skips the yanked version"    0 "added hello-orchard ^1.0.0 (1.0.0)" out -- add hello-orchard
check "the older version runs"          0 "^Hello, Ada" out -- run .

# Checksums: a bundle that changed after it was published is refused.
rm -rf "$tmp/home"
cp "$registry/packages/hello-orchard/1.0.0.plumpkg" "$tmp/good.plumpkg"
sed 's/from the orchard/from elsewhere/' "$tmp/good.plumpkg" >"$registry/packages/hello-orchard/1.0.0.plumpkg"
check "a checksum mismatch is refused"  1 "E0920.*checksum mismatch for hello-orchard 1.0.0" err -- install
expect "nothing was unpacked"           sh -c "! test -e '$tmp/home/orchard/hello-orchard/1.0.0'"
cp "$tmp/good.plumpkg" "$registry/packages/hello-orchard/1.0.0.plumpkg"
check "the good bundle installs"        0 "installed hello-orchard 1.0.0" out -- install

# Path traversal: a bundle, with a correct checksum, that would write outside.
mkdir -p "$registry/packages/evil"
printf '{"files":{"../../escaped.plum":"drop(1)\\n","lib.plum":"","plum.toml":"[package]\\nname = \\"evil\\"\\nversion = \\"1.0.0\\"\\nentry = \\"lib.plum\\"\\n"},"format":1,"name":"evil","version":"1.0.0"}' \
    >"$registry/packages/evil/1.0.0.plumpkg"
printf 'seed hash\nseed fs\ndrop(hash.sha256(fs.read("%s")))\n' "$(native "$registry/packages/evil/1.0.0.plumpkg")" >"$tmp/sha.plum"
esha=$("$PLUM" run "$(native "$tmp/sha.plum")" | tr -d '\r')
printf '{"version": "1.0.0", "sha256": "%s", "dependencies": {}, "plum": "*", "published": "2026-11-15T00:00:00Z", "yanked": false}\n' "$esha" >"$tmp/entries/evil"
index evil
check "path traversal is refused"       1 "E0921.*unsafe path '../../escaped.plum'" err -- add evil
expect "nothing escaped"                sh -c "! test -e '$tmp/home/escaped.plum' && ! test -e '$tmp/home/orchard/escaped.plum' && ! test -e '$tmp/home/orchard/evil/1.0.0'"
expect "plum.toml is as it was"         sh -c '! grep -q evil plum.toml'

# Dependencies of dependencies, and conflicts that name both sides.
check "a dependency's dependency"       0 "added paint ^1.0.0 (1.0.0)" out -- add paint
expect "the lock has colors 1.0.0"      grep -q '^dependencies = \["colors"\]$' plum.lock
check "a version conflict"              1 "E0922.*paint 1.0.0 requires colors ^1.0, brush 1.0.0 requires colors ^2.0" err -- add brush
check "no such package"                 1 "E0923.*no package 'nothing-here'" err -- add nothing-here
check "a bad name"                      1 "E0905" err -- add Bad_Name
check "a stdlib name"                   1 "E0905.*standard module" err -- add json
check "a bad range"                     1 "E0907" err -- add colors@^x

# Search, and publishing's link.
check "search by keyword"               0 "^hello-orchard 1.1.0  A friendly greeting" out -- search greeting
check "search finds nothing"            0 "no packages match" out -- search zebra
cd "$P/hello-orchard-1.0.0" || exit 1
check "publish prints the link"         0 "issues/new?template=publish.yml&title=Publish%20hello-orchard%201.0.0&package=hello-orchard&repository=https%3A%2F%2Fgithub.com%2Fplumlang%2Fregistry&tag=v1.0.0" out -- publish
cd "$app" || exit 1

# ---- plum package refuses what can't be published ----

bad="$tmp/bad"
mkdir -p "$bad"
cd "$bad" || exit 1
printf '[package]\nname = "bad-pkg"\nversion = "1.0.0"\nentry = "lib.plum"\n' >plum.toml
printf 'drop(1 +)\n' >lib.plum
check "package runs plum check"         1 "E0925.*errors in the package's files" err -- package
printf 'drop(1)\n' >lib.plum
printf 'x\000y' >bin.plum
check "package refuses binary files"    1 "E0925.*'bin.plum' isn't UTF-8 text" err -- package
rm bin.plum
printf 'drop(2)\n' >"a b.plum"
check "package refuses odd paths"       1 "E0925.*'a b.plum' can't be in a package" err -- package
rm "a b.plum"
awk 'BEGIN { for (i = 0; i < 40000; i++) print "# padding padding padding" }' >big.plum
check "package refuses over 1 MB"       1 "E0925.*the limit is 1 MB" err -- package
rm big.plum
check "package writes the bundle"       0 "packaged bad-pkg 1.0.0: 2 files" out -- package
expect "the bundle is there"            test -f bad-pkg-1.0.0.plumpkg
check "publish needs a repository"      1 "E0925.*repository" err -- publish

# ---- plum.toml is read strictly ----

manifest() {
    printf '%b' "$2" >plum.toml
    check "$1" 3 "$3" err -- run .
}
manifest "a number"         '[package]\nname = "x1"\nversion = 1\n' "plum.toml:3:11: error\[E0901\]"
manifest "a key twice"      '[package]\nname = "x1"\nname = "x2"\n' "plum.toml:3:1: error\[E0902\]"
manifest "an unknown key"   '[package]\nnmae = "x1"\nname = "x1"\nversion = "1.0.0"\nentry = "lib.plum"\n' "E0903.*'nmae'"
manifest "a missing entry"  '[package]\nname = "x1"\nversion = "1.0.0"\n' "E0904.*no entry"
manifest "a bad name"       '[package]\nname = "X"\nversion = "1.0.0"\nentry = "lib.plum"\n' "E0905"
manifest "a bad version"    '[package]\nname = "x1"\nversion = "1.0"\nentry = "lib.plum"\n' "plum.toml:3:11: error\[E0906\]"
manifest "a bad range"      '[package]\nname = "x1"\nversion = "1.0.0"\nentry = "lib.plum"\n[dependencies]\ncolors = "<2"\n' "E0907"
manifest "an unsafe entry"  '[package]\nname = "x1"\nversion = "1.0.0"\nentry = "../lib.plum"\n' "E0911"
cd "$app" || exit 1
printf 'garbage\n' >plum.lock
check "a broken plum.lock"              1 "E0910" err -- run .
rm plum.lock
check "no plum.lock"                    1 "E0909.*no plum.lock" err -- run .

# ---- plum new: a project that runs, tests, checks, formats and packages ----

before_new=$(pwd)
mkdir -p "$tmp/new"
cd "$tmp/new" || exit 1
check "new: a project"              0 "created my-app/ with 8 files" out -- new my-app
for f in plum.toml README.md .gitignore .gitattributes .github/workflows/ci.yml src/main.plum src/my_app.plum src/my_app_test.plum; do
    expect "new: $f exists" test -f "my-app/$f"
done
check "new: an existing name"       1 "E0926.*'my-app' already exists" err -- new my-app
check "new: an invalid name"        1 "E0926.*'My_App' can't be a project's name" err -- new My_App
check "new: a standard module name" 1 "E0926.*'json' can't be a project's name" err -- new json
check "new: no name"                2 "takes the project's name" err -- new
cd "$tmp/new/my-app" || exit 1
check "new: runs"                   0 "^Hello, world!$" out -- run .
check "new: its test passes"        0 "1 passed, 0 failed" out -- test
expect "new: checks"                "$PLUM" check .
expect "new: is formatted"          "$PLUM" fmt --check .
check "new: packages"               0 "packaged my-app 0.1.0" out -- package --out "$(native "$tmp/new")"
cd "$before_new" || exit 1

# ---- Over HTTP, when python3 can serve the index ----

if command -v python3 >/dev/null 2>&1 && [ -z "$PLUM_ORCHARD_NO_HTTP" ]; then
    python3 -u -m http.server 0 --bind 127.0.0.1 --directory "$registry" >"$tmp/server" 2>&1 &
    server=$!
    port=
    tries=0
    while [ -z "$port" ] && [ "$tries" -lt 40 ]; do
        sleep 1
        port=$(sed -n 's/.* port \([0-9][0-9]*\).*/\1/p' "$tmp/server" | head -n 1)
        tries=$((tries + 1))
    done
    if [ -n "$port" ]; then
        PLUM_ORCHARD="http://127.0.0.1:$port"
        rm -rf "$tmp/home"
        check "search over HTTP"        0 "^colors 2.0.0" out -- search color
        check "install over HTTP"       0 "installed colors 1.0.0" out -- install
        check "a missing package over HTTP" 1 "E0923.*no package 'nothing-here'" err -- add nothing-here
    else
        echo "orchard: the HTTP server didn't start; HTTP tests skipped"
    fi
fi

echo "orchard: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
