# Orchard: packages

Orchard is Plumlang's package registry. A package is a directory with a
`plum.toml`; Orchard keeps an index of published packages, and the `plum`
command adds them to a project, locks their exact versions and checksums,
and installs them. The language side, `seed weather`, is spec section 9.3.

Orchard begins as a git repository,
[plumlang/registry](https://github.com/plumlang/registry): an index of
JSON files and the packages' bundles, with no server and no accounts. The
client reads one base URL, so a CDN or a hosted API can take its place
later without changing any command.

## Starting a project

```sh
plum new my-app
cd my-app
plum run .        # Hello, world!
plum test         # 1 passed, 0 failed
```

`plum new NAME` makes a directory NAME holding a project that runs,
passes its test, is formatted, and packages as it is:

| File | What |
|---|---|
| `plum.toml` | The package: `name`, `version = "0.1.0"`, `entry = "src/main.plum"`, `lib`, and the `plum` versions it works with |
| `src/main.plum` | The program, what `plum run .` runs |
| `src/NAME.plum` | The library, what `seed NAME` gives a project that depends on it (`-` becomes `_`) |
| `src/NAME_test.plum` | A test of it, run by `plum test` |
| `README.md` | How to run, test, format and add packages |
| `.gitignore` | Ignores the bundles `plum package` writes |
| `.gitattributes` | LF line endings for `.plum` files, so `plum fmt --check` agrees on Windows checkouts |
| `.github/workflows/ci.yml` | GitHub Actions on Linux, macOS and Windows: [setup-plum](https://github.com/plumlang/setup-plum), then `plum check .`, `plum fmt --check .` and `plum test` |

NAME follows the package-name rules below. `plum new` writes nothing if
NAME is invalid or already exists (E0926). The same project is a GitHub
template: [plumlang/template](https://github.com/plumlang/template),
"Use this template".

## Using packages

Run the commands in the project's directory, the one with `plum.toml`:

```sh
plum search weather          # find packages
plum add weather             # add the newest version, as ^VERSION
plum add weather@~1.2        # or a range of your choice
plum install                 # install exactly what plum.lock lists
plum remove weather
```

`plum add` resolves the package and everything it needs, installs it,
writes `plum.lock`, and only then adds the line to `[dependencies]` in
`plum.toml` (the rest of the file is left as it was). If anything fails,
neither file changes.

Then, in any file of the project:

```plum
seed weather                 # the package's lib file: weather.forecast(...)
seed weather.client          # its client.plum: client.get(...)
seed weather.api.v2          # its api/v2.plum
seed weather { forecast }    # names brought in directly
```

- **Standard modules win.** `seed json` is always the standard module, and
  so is anything under a standard module's name (`seed net.whatever`).
  Orchard refuses packages with those names.
- **`-` is written `_`.** Package names may have `-`, which a `seed`
  can't; seed `hello-orchard` as `seed hello_orchard`. Names never have
  `_`, so this is never ambiguous.
- **Only declared packages.** A module may seed only the packages its
  `plum.toml` lists: a project's files the project's dependencies, a
  package's files that package's own. Anything else is error E0908, with
  the hint `plum add NAME`. A declared package that isn't installed is
  E0909: run `plum install`.
- The project is the nearest directory with a `plum.toml`, from the file
  being run upwards.

Commit `plum.toml` and `plum.lock`. Someone who clones the project runs
`plum install` and gets the same versions, checked against the same
checksums.

## The manifest: plum.toml

```toml
[package]
name = "weather"            # required
version = "1.2.0"           # required: MAJOR.MINOR.PATCH
entry = "main.plum"         # required: what `plum run .` runs
lib = "lib.plum"            # what `seed weather` imports (default: entry)
description = "Forecasts from open weather APIs"
license = "MIT"
authors = ["Ada <ada@example.com>"]
repository = "https://github.com/ada/weather"   # needed to publish
keywords = ["weather", "http"]
plum = ">=0.1"              # the Plumlang versions it works with

[dependencies]
colors = "^1.0"
```

`plum.toml` is read by a small, strict part of TOML: `[tables]`,
`key = "string"` (or `'literal'`), lists of strings, which may run over
several lines, and `#` comments. Every value is a string or a list of
strings; there are no numbers, booleans, inline tables or arrays of
tables. A misspelt key is an error, not ignored. Every problem is
reported at its line and column with a code (E0901 to E0911 in
[errors.md](errors.md)).

- **Names**: 2 to 40 lowercase letters, digits and single `-`, starting
  with a letter and not ending with `-`. Not a standard module's name,
  `plum`, `std`, `stdlib` or `orchard`, and not a keyword once `-`
  becomes `_` (so not `tree`).
- **Versions** are exactly three numbers, `MAJOR.MINOR.PATCH`, without
  leading zeros. There are no pre-release or build labels (`1.0.0-beta`).
- **`entry` and `lib`** name `.plum` files inside the package.
- **Description**: at most 300 bytes. **Keywords**: at most 10, each 1 to
  30 lowercase letters, digits and `-`.
- **`plum`** is a version range. Installing a package whose range doesn't
  include the running `plum` prints a warning.

### Version ranges

One requirement per dependency:

| Range | Means | Example |
|---|---|---|
| `^1.2.3` | the same left-most non-zero number | `>=1.2.3, <2.0.0`; `^0.2.3` is `<0.3.0`; `^0.0.3` is `<0.0.4` |
| `~1.2.3` | the same minor version | `>=1.2.3, <1.3.0`; `~1` is `<2.0.0` |
| `=1.2.3` | exactly | `=1.2` is any `1.2.x` |
| `>=1.2` | at least | |
| `*` | any version | |
| `1.2` | the same as `^1.2` | |

Parts left out are zeros: `^1.2` is `^1.2.0`. The [`semver`](stdlib/semver.md)
module reads ranges with the same code, so a program can check them too.

### Resolution

Each package is installed in one version, shared by everything in the
project that needs it. For each package, `plum` takes every requirement on
it (from the project and from the packages it already chose) and picks:

1. the version in `plum.lock`, if there is one and it satisfies them all
   (so adding a package doesn't upgrade the others);
2. otherwise the highest version that satisfies them all and isn't
   yanked.

It repeats until nothing changes. When no version fits, error E0922
names every requirer and its range:

```text
plum: error[E0922]: version conflict for 'colors': paint 1.0.0 requires colors ^1.0, brush 1.0.0 requires colors ^2.0; no version satisfies them all
  hint: change one of the ranges so they overlap
```

## The lock file: plum.lock

Written by `plum add`, `plum remove` and `plum install`; never edited by
hand. TOML, one table per package, in name order:

```toml
# plum.lock: the exact packages this project uses, written by plum.
# Commit it with the project; don't edit it by hand.

[packages.colors]
version = "1.0.0"
sha256 = "5b0c...e1"
dependencies = []

[packages.paint]
version = "1.0.0"
sha256 = "9f3a...07"
dependencies = ["colors"]
```

`plum install` installs exactly these, verifying each bundle's sha256
before unpacking it. If `plum.toml` changed so the lock no longer fits
(a dependency added by hand, a range the locked version is outside), it
resolves again, keeping every locked version that still fits.

## The cache

Packages are unpacked once and shared by every project:

| | Directory |
|---|---|
| `PLUM_HOME` set | `$PLUM_HOME/orchard/NAME/VERSION/` |
| Linux, macOS | `~/.plum/orchard/NAME/VERSION/` |
| Windows | `%LOCALAPPDATA%\plum\orchard\NAME\VERSION\` (else `%USERPROFILE%\.plum\orchard\...`) |

A version is unpacked into `VERSION.part`, checked, and then renamed into
place, so a failed install leaves nothing half-written. The file
`.orchard-sha256` in each version records the bundle it came from;
`plum install` downloads again when it doesn't match the lock. The cache
can be deleted at any time.

## The registry

`PLUM_ORCHARD` sets the base URL; the default is
`https://raw.githubusercontent.com/plumlang/registry/main`. It may be
`https://`, `http://` (for tests and mirrors) or `file:///path` (an index
on disk). Redirects are followed, up to five.

```text
index/we/weather.json          the package's index file (first two letters of the name)
packages/weather/1.2.0.plumpkg the bundles
search.json                    every package, for search and the website
```

### Index files

```json
{
  "name": "weather",
  "owners": ["ada"],
  "description": "Forecasts from open weather APIs",
  "repository": "https://github.com/ada/weather",
  "keywords": ["weather", "http"],
  "versions": [
    {
      "version": "1.2.0",
      "sha256": "64 lowercase hex digits: the bundle's SHA-256",
      "dependencies": {"colors": "^1.0"},
      "plum": ">=0.1",
      "published": "2026-11-15T10:00:00Z",
      "yanked": false
    }
  ]
}
```

`owners` are GitHub logins: the first publisher owns the name, and only
owners publish new versions or yank. `plum` reads `name` and `versions`;
the rest is for people and the website.

### search.json

```json
{"packages": [{"name": "weather", "version": "1.2.0", "description": "...", "keywords": ["weather"]}]}
```

`version` is the newest version that isn't yanked. `plum search` matches
every word given against the name, description and keywords, ignoring
case.

### Bundles: .plumpkg

A bundle is one UTF-8 JSON object:

```json
{"files":{"lib.plum":"...","plum.toml":"..."},"format":1,"name":"weather","version":"1.2.0"}
```

- **Byte for byte reproducible.** Every object's keys are in byte order,
  with no spaces; files are their text with `\r\n` made `\n`. The same
  source gives the same bytes, and so the same sha256, on every platform.
- **Text only.** `.plum` files anywhere in the package, and `plum.toml`,
  `README.md` and `LICENSE*` at its top; each valid UTF-8. Directories
  starting with `.` (`.git`) and every other file are left out.
- **At most 1 MB** (1,048,576 bytes) in all.
- **Safe paths.** `/` between parts; each part letters, digits, `_`, `-`
  and `.`, not starting with `.` or `-` or ending with `.`; no `..`,
  absolute paths, backslashes, drive letters or device names (`CON`,
  `NUL`); at most 16 levels and 200 characters; no two paths that differ
  only in case.

Installing checks, in order: the bundle's sha256 against the lock (E0920);
its size, format, name and version (E0924); every path (E0921), all before
writing anything; then the unpacked `plum.toml`.

## Publishing

1. Make the package: a `plum.toml` with a name, version, entry, and
   `repository = "https://github.com/OWNER/REPO"`.
2. `plum package` checks it: the manifest, `plum check` on every `.plum`
   file (installing its dependencies first), the file set and the size.
   It writes `NAME-VERSION.plumpkg` and prints its sha256.
3. Tag the release in your repository and push the tag.
4. `plum publish` (or `plum publish --tag TAG`; the default is
   `vVERSION`) runs the same checks and prints a link that opens a
   prefilled publish request on plumlang/registry: the package, the
   repository and the tag. Submit it while signed in to GitHub.

A workflow in plumlang/registry checks out the tag, runs the latest
released `plum package` on it, checks that the name is free (or that you
own it) and that the version is new and higher than the last, then
commits the bundle and the index, and replies on the issue. The bundle
Orchard serves is the one it built, not one you uploaded.

**Yanking.** An owner opens a "yank" issue on plumlang/registry. The
version stays installable from lock files that already have it, but is
never chosen again. Versions are never deleted or replaced.

## How it is built

The commands are a Plumlang program built into `plum`,
`src/stdlib/plum/plum_orchard.plum`, using `net.http`, `json`, `fs` and
`hash`. What must be C is in `src/orchard/`: the TOML subset, the
manifest and lock file, versions, ranges, names and paths, finding a
package for `seed` (`packages.c`, used by the loader), and an internal
module, `plum.pkg`, that gives the program those, plus `plum check`.
Internal modules (`plum.*`) can be seeded only by modules built into
`plum`. The tests are `tests/orchard.sh`, which builds an index from
`tests/orchard/packages` and runs every command against it offline, and
the unit tests in `tests/unit/test_orchard.c`.
