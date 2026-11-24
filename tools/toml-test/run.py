#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
"""Run the toml module against the toml-test suite's TOML 1.0 cases.

    git clone https://github.com/toml-lang/toml-test
    python3 tools/toml-test/run.py path/to/toml-test ./plum

Every valid case must parse to its expected value, and every invalid case
must be a ParseError. toml.parse gives dates and times as their text, so
those are compared as text (fractions of a second by value). The suite is
not vendored here; this runs it from a checkout.
"""
import json
import math
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def date_text(s):
    s = s.replace("t", "T").replace("z", "Z")
    if len(s) > 10 and s[10] == " ":
        s = s[:10] + "T" + s[11:]
    # 17:45:56.6Z and 17:45:56.600Z are the same time
    if "." in s:
        head, _, rest = s.partition(".")
        digits = ""
        while rest and rest[0].isdigit():
            digits, rest = digits + rest[0], rest[1:]
        s = head + ("." + digits.rstrip("0") if digits.rstrip("0") else "") + rest
    return s


def differs(want, got, path):
    if isinstance(want, dict) and set(want) == {"type", "value"} and isinstance(want["value"], str):
        if not (isinstance(got, dict) and "type" in got):
            return f"{path}: want {want}, got {got}"
        wt, wv, gt, gv = want["type"], want["value"], got["type"], got["value"]
        if wt.startswith(("datetime", "date", "time")):
            ok = gt == "string" and date_text(gv) == date_text(wv)
        elif wt == "float":
            ok = gt == "float" and (
                (math.isnan(float(wv)) and math.isnan(float(gv))) or float(wv) == float(gv)
            )
        elif wt == "integer":
            ok = gt == "integer" and int(wv) == int(gv)
        else:
            ok = wt == gt and wv == gv
        return None if ok else f"{path}: want {wt} {wv!r}, got {gt} {gv!r}"
    if isinstance(want, dict):
        if not isinstance(got, dict) or sorted(want) != sorted(got):
            return f"{path}: want keys {sorted(want)}, got {got if not isinstance(got, dict) else sorted(got)}"
        for k in want:
            d = differs(want[k], got[k], f"{path}.{k}")
            if d:
                return d
        return None
    if isinstance(want, list):
        if not isinstance(got, list) or len(want) != len(got):
            return f"{path}: want {len(want)} elements"
        for i, (a, b) in enumerate(zip(want, got)):
            d = differs(a, b, f"{path}[{i}]")
            if d:
                return d
        return None
    return f"{path}: unexpected {want!r}"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    suite, plum = sys.argv[1], sys.argv[2]
    with open(os.path.join(suite, "tests", "files-toml-1.0.0")) as f:
        cases = [line.strip() for line in f if line.strip().endswith(".toml")]
    failed = []
    counts = {"valid": 0, "invalid": 0}
    for case in cases:
        kind = case.split("/")[0]
        path = os.path.join(suite, "tests", case)
        counts[kind] += 1
        run = subprocess.run([plum, "run", os.path.join(HERE, "encode.plum"), path],
                             capture_output=True, timeout=60)
        err = run.stderr.decode(errors="replace").strip()
        if kind == "invalid":
            if run.returncode == 0:
                failed.append((case, "accepted"))
            elif "ParseError" not in err:
                failed.append((case, err[:200]))
            continue
        if run.returncode != 0:
            failed.append((case, err[:200]))
            continue
        with open(path[:-5] + ".json") as f:
            want = json.load(f)
        d = differs(want, json.loads(run.stdout.decode()), "")
        if d:
            failed.append((case, d))
    for case, why in failed:
        print(f"FAIL {case}: {why}")
    print(f"toml-test: {counts['valid']} valid and {counts['invalid']} invalid cases, "
          f"{len(failed)} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
