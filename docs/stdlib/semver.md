# semver

[Semantic versions](https://semver.org) (2.0.0) and version ranges:
parse, compare and sort versions, check them against a range, and bump
them. A range is written as in `plum.toml` ([Orchard](../orchard.md#version-ranges)),
and is read by the same code, so it means the same here as there.

```plum
seed semver

drop(semver.compare("1.10.0", "1.9.2"))                 # 1
drop(semver.sort(["2.0.0", "1.0.0", "1.0.0-rc.1"]))     # ["1.0.0-rc.1", "1.0.0", "2.0.0"]
drop(semver.satisfies("1.4.2", "^1.2"))                 # good
drop(semver.max_satisfying(["1.2.0", "1.5.1", "2.0.0"], "~1.5"))  # 1.5.1
drop(semver.bump("1.4.2", "minor"))                     # 1.5.0

plant v = semver.parse("2.0.0-beta.3+build.7")
drop(int(v["major"]), v["prerelease"], v["build"])      # 2 beta.3 build.7
```

## Versions

A version is `MAJOR.MINOR.PATCH`: three numbers without leading zeros,
each at most 999,999,999. It may go on with `-` and a prerelease
(`1.0.0-beta.2`) and `+` and build metadata (`1.0.0+20261124`), each
dot-separated identifiers of letters, digits and `-`; a prerelease
identifier that is a number has no leading zeros. Nothing else is taken:
no `v` in front, no spaces, no `1.2` for `1.2.0`. `is_valid` checks a
text without throwing.

`parse` gives the parts as text, in a map: `major`, `minor`, `patch`,
`prerelease` and `build`, the last two `""` when the version has none.

## Order

`compare`, `sort` and `max_satisfying` order versions as semver.org
does: by the three numbers, then a prerelease comes before its release
(`1.0.0-rc.1` before `1.0.0`), and prereleases compare identifier by
identifier, numbers by value and before words, words in ASCII order, and
a shorter list first if the rest is equal:

    1.0.0-alpha < 1.0.0-alpha.1 < 1.0.0-alpha.beta < 1.0.0-beta
      < 1.0.0-beta.2 < 1.0.0-beta.11 < 1.0.0-rc.1 < 1.0.0

Build metadata doesn't count: `compare("1.0.0+a", "1.0.0+b")` is `0`.
`sort` is stable, so such versions keep the order they came in.

## Ranges

One requirement, as `plum.toml` writes them:

| Range | Means | Example |
|---|---|---|
| `^1.2.3` | the same left-most non-zero number | `>=1.2.3, <2.0.0`; `^0.2.3` is `<0.3.0`; `^0.0.3` is `<0.0.4` |
| `~1.2.3` | the same minor version | `>=1.2.3, <1.3.0`; `~1` is `<2.0.0` |
| `=1.2.3` | exactly | `=1.2` is any `1.2.x` |
| `>=1.2` | at least | |
| `*` | any version | |
| `1.2` | the same as `^1.2` | |

Parts left out are zeros, and spaces may come before the version or
after the operator. Compound ranges (`>=1.0, <2.0`, `||`), `>`, `<`
and `1.x` aren't taken, as in `plum.toml`.

Beyond `plum.toml`, whose versions never have one, a range here may name
a prerelease on a full version: `^1.0.0-beta`, `>=2.0.0-rc.1`,
`=1.0.0-beta` (that prerelease only). A version with a prerelease is in a
range only when the range's own version has a prerelease on the same
`MAJOR.MINOR.PATCH`, as npm and Cargo do: `^1.0.0` never picks
`2.0.0-alpha` or `1.1.0-beta`, while `^1.0.0-beta` takes `1.0.0-beta.2`
and `1.0.0`, but not `1.1.0-beta`.

## Bumping

`bump(version, part)` gives the next `"major"`, `"minor"` or `"patch"`
version, dropping the prerelease and build. A prerelease bumps to its
own release when that is the next version of the part asked for, as `npm
version` does: `1.2.3-beta` bumps its patch to `1.2.3`, `1.2.0-beta` its
minor to `1.2.0`, `2.0.0-rc.1` its major to `2.0.0`.

## Errors

- `ValueError` from every function but `is_valid` and `is_valid_range`
  when a version or range isn't one, like
  `semver.parse: "1.2" isn't a version; write MAJOR.MINOR.PATCH, like "1.4.2" or "2.0.0-beta.1"`.
- `ValueError` from `bump` for a part other than `"major"`, `"minor"` or
  `"patch"`, or a number that would pass 999,999,999.

## Reference

<!-- reference:begin -->
| Name | Signature | What it does |
|---|---|---|
| `parse` | `parse(version: str) -> map[str, str]` | the parts of a version: major, minor, patch, prerelease and build ("" when it has none) |
| `is_valid` | `is_valid(version: str) -> bool` | is the text a semantic version, MAJOR.MINOR.PATCH with an optional -prerelease and +build? |
| `compare` | `compare(a: str, b: str) -> int` | -1, 0 or 1 as a comes before, with or after b; a prerelease comes before its release, and build metadata doesn't count |
| `sort` | `sort(versions: list[str]) -> list[str]` | the versions, oldest first (equal ones keep their order) |
| `satisfies` | `satisfies(version: str, range: str) -> bool` | is the version in the range (^1.2, ~1.2.3, =1.2.3, >=1.2, *, as in plum.toml)? |
| `max_satisfying` | `max_satisfying(versions: list[str], range: str) -> str?` | the highest of the versions in the range, or none |
| `is_valid_range` | `is_valid_range(range: str) -> bool` | is the text a version range satisfies takes? |
| `bump` | `bump(version: str, part: str) -> str` | the next "major", "minor" or "patch" version; a prerelease bumps to its release |
<!-- reference:end -->
