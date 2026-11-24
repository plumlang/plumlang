#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
"""Compare a bench/run.py --json result with a baseline (the last on main).

A benchmark regresses when its Plumlang time is more than --threshold
percent (default 15) slower than the baseline's, and at least --min-delta
seconds (default 0.02) slower, so that the noise of a very short run
can't fail the check alone. Python, Lua and Node.js times are shown for
reference and never fail it.

Prints a Markdown table (and appends it to --summary FILE, for
$GITHUB_STEP_SUMMARY). Exits 1 if anything regressed, unless --warn-only.
With no baseline (the first run, or an expired artifact) it passes.

Usage: python3 bench/compare.py CURRENT.json [BASELINE.json]
           [--threshold PCT] [--min-delta SECONDS] [--summary FILE] [--warn-only]
"""
import argparse
import json
import os
import sys


def load(path):
    if not path or not os.path.exists(path):
        return None
    with open(path) as f:
        return json.load(f)


def seconds(t):
    return f"{t:.3f} s" if t is not None else "-"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("current")
    ap.add_argument("baseline", nargs="?")
    ap.add_argument("--threshold", type=float, default=15.0, help="percent")
    ap.add_argument("--min-delta", type=float, default=0.02, help="seconds")
    ap.add_argument("--summary", help="append the Markdown table to this file")
    ap.add_argument("--warn-only", action="store_true")
    args = ap.parse_args()

    cur = load(args.current)
    if cur is None:
        print(f"compare.py: no results in {args.current}", file=sys.stderr)
        return 2
    base = load(args.baseline)
    others = [k for k in cur["versions"] if k != "plum"]

    lines = ["## Benchmarks", ""]
    if base is None:
        lines.append("No baseline from `main` yet, so nothing is compared; "
                     "this run's results become one once it is on `main`.")
    else:
        lines.append(f"Plumlang against the last `main` run: a benchmark fails when it is "
                     f"more than {args.threshold:g}% and {args.min_delta:g} s slower. "
                     f"Fastest of {cur['runs']} runs.")
    lines.append("")
    head = "| Benchmark | main | this run | change |"
    rule = "|---|---|---|---|"
    for name in others:
        head += f" {name} |"
        rule += "---|"
    lines += [head, rule]

    regressed = []
    for bench, row in sorted(cur["seconds"].items()):
        now = row["plum"]
        before = (base or {}).get("seconds", {}).get(bench, {}).get("plum")
        if before:
            pct = (now - before) / before * 100
            change = f"{pct:+.1f}%"
            if pct > args.threshold and now - before >= args.min_delta:
                change += " **slower**"
                regressed.append(f"{bench} {pct:+.1f}% ({before:.3f} s -> {now:.3f} s)")
        else:
            change = "new" if base else "-"
        line = f"| {bench} | {seconds(before)} | {seconds(now)} | {change} |"
        for name in others:
            t = row.get(name)
            line += f" {seconds(t)} ({t / now:.2f}x) |" if t else " - |"
        lines.append(line)

    lines.append("")
    versions = ", ".join(f"{k}: {v}" for k, v in cur["versions"].items())
    lines.append(f"{cur.get('machine', '')}; {versions}. "
                 "(Nx) is how many times longer the other language took than Plumlang.")
    if regressed:
        lines += ["", "**Slower than `main`:** " + "; ".join(regressed)]
    text = "\n".join(lines) + "\n"
    print(text)
    if args.summary:
        with open(args.summary, "a") as f:
            f.write(text)
    if regressed and not args.warn_only:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
