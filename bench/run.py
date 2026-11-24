#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
"""Time the benchmarks in bench/ against Python, Lua and Node.js.

Every NAME.plum here may have twins in python/NAME.py, lua/NAME.lua and
node/NAME.mjs. Each program runs once to check that it prints exactly
what the Plumlang version prints (a benchmark whose output differs is
reported as wrong and not timed), then `--runs` more times (default 3);
the fastest run counts.

Interpreters that aren't installed are skipped with a note. Lua is the
first of lua5.4, lua, luajit, lua5.3 found on PATH, or --lua PATH.

--json FILE writes the results (seconds, per benchmark and interpreter,
plus the versions used) for bench/compare.py and the CI job.

--baseline PLUM times a second Plumlang binary (main's, in CI) against
the first on the same machine: each benchmark runs the two in turn,
--pairs times (default 7), swapping which goes first every pair so that
neither always runs warm, and the median of each binary's runs counts.
Interleaving puts both under the same load, so a slow or busy machine
slows both alike. The results then hold "base" (and every run, under
"samples") beside "plum" for bench/compare.py. A benchmark the baseline
can't run (one that needs something it doesn't have yet) is timed for
this build alone.

The exit status is 1 if any program failed or printed the wrong output.

Usage: python3 bench/run.py [./plum] [--runs N] [--only NAME[,NAME...]]
                            [--json FILE] [--lua PATH] [--plum-only]
                            [--baseline PLUM [--pairs N]]
"""
import argparse
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

# name, folder, file extension, executable candidates, extra arguments
OTHERS = [
    ("python3", "python", ".py", ["python3"], ["-I"]),
    ("lua", "lua", ".lua", ["lua5.4", "lua", "luajit", "lua5.3"], []),
    ("node", "node", ".mjs", ["node"], []),
]


def run_once(cmd):
    start = time.perf_counter()
    done = subprocess.run(cmd, capture_output=True, text=True)
    elapsed = time.perf_counter() - start
    if done.returncode != 0:
        return None, (done.stderr or done.stdout).strip()
    return elapsed, done.stdout


def best_time(cmd, runs):
    best = None
    for _ in range(runs):
        elapsed, out = run_once(cmd)
        if elapsed is None:
            return None, out
        best = elapsed if best is None else min(best, elapsed)
    return best, None


def paired_times(cmd, base_cmd, pairs):
    """Run cmd and base_cmd in turn, pairs times each; their run times."""
    mine, base = [], []
    for i in range(pairs):
        order = [(cmd, mine), (base_cmd, base)]
        if i % 2:
            order.reverse()
        for c, into in order:
            elapsed, out = run_once(c)
            if elapsed is None:
                return None, None, out
            into.append(elapsed)
    return mine, base, None


def version_of(exe, flag):
    try:
        done = subprocess.run([exe, flag], capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return "?"
    text = (done.stdout or done.stderr).strip()
    return text.splitlines()[0] if text else "?"


def find_interpreters(args):
    found = {}
    for name, folder, ext, candidates, extra in OTHERS:
        if args.plum_only:
            continue
        if name == "lua" and args.lua:
            candidates = [args.lua]
        exe = next((shutil.which(c) for c in candidates if shutil.which(c)), None)
        if exe is None:
            print(f"note: {name} not found ({', '.join(candidates)}); skipping it")
            continue
        flag = "-v" if name == "lua" else "--version"
        found[name] = {
            "exe": exe,
            "folder": folder,
            "ext": ext,
            "extra": extra,
            "version": version_of(exe, flag),
        }
    return found


def cell(seconds, plum_seconds):
    if seconds is None:
        return f"{'-':>9} {'':>7}"
    return f"{seconds:8.3f}s {plum_seconds and seconds / plum_seconds or 0:6.2f}x"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("plum", nargs="?", default="./plum")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--only", help="comma-separated benchmark names")
    ap.add_argument("--json", help="write the results to this file")
    ap.add_argument("--lua", help="the Lua interpreter to use")
    ap.add_argument("--plum-only", action="store_true", help="time Plumlang alone")
    ap.add_argument("--baseline", help="a second plum to time against the first, interleaved")
    ap.add_argument("--pairs", type=int, default=7, help="runs of each plum with --baseline")
    args = ap.parse_args()

    names = sorted(f[:-5] for f in os.listdir(HERE) if f.endswith(".plum"))
    if args.only:
        wanted = set(args.only.split(","))
        names = [n for n in names if n in wanted]
    others = find_interpreters(args)

    header = f"{'benchmark':<12} {'plum':>9}"
    if args.baseline:
        header += f" {'base':>9} {'change':>7}"
    for name in others:
        header += f" {name:>9} {'':>7}"
    print(header)

    results = {}
    bad = False
    for bench in names:
        plum_cmd = [args.plum, "run", os.path.join(HERE, bench + ".plum")]
        first, expected = run_once(plum_cmd)
        base_ok = False
        if first is not None and args.baseline:
            base_cmd = [args.baseline, *plum_cmd[1:]]
            base_ok = run_once(base_cmd)[0] is not None
        if first is None:
            p_time, err = None, expected
        elif base_ok:
            mine, base, err = paired_times(plum_cmd, base_cmd, args.pairs)
            p_time = mine and statistics.median(mine)
        elif args.baseline:
            mine, err = [], None
            for _ in range(args.pairs):
                elapsed, out = run_once(plum_cmd)
                if elapsed is None:
                    mine, err = None, out
                    break
                mine.append(elapsed)
            p_time = mine and statistics.median(mine)
        else:
            p_time, err = best_time(plum_cmd, args.runs)
        if p_time is None:
            print(f"{bench:<12} failed: {err}")
            bad = True
            continue
        row = {"plum": p_time}
        line = f"{bench:<12} {p_time:8.3f}s"
        if base_ok:
            b_time = statistics.median(base)
            row["base"] = b_time
            row["samples"] = {"plum": mine, "base": base}
            line += f" {b_time:8.3f}s {(p_time - b_time) / b_time * 100:+6.1f}%"
        elif args.baseline:
            row["samples"] = {"plum": mine}
            line += f" {'-':>9} {'new':>7}"
        for name, it in others.items():
            path = os.path.join(HERE, it["folder"], bench + it["ext"])
            if not os.path.exists(path):
                line += " " + cell(None, p_time)
                continue
            cmd = [it["exe"], *it["extra"], path]
            elapsed, out = run_once(cmd)
            if elapsed is None:
                print(f"{bench:<12} {name} failed: {out}")
                bad = True
                line += " " + cell(None, p_time)
                continue
            if out != expected:
                print(f"{bench:<12} WRONG: plum printed {expected!r}, {name} {out!r}")
                bad = True
                line += " " + cell(None, p_time)
                continue
            t, err = best_time(cmd, args.runs)
            row[name] = t
            line += " " + cell(t, p_time)
        results[bench] = row
        print(line)

    if args.json:
        doc = {
            "runs": args.runs,
            **({"pairs": args.pairs,
                "base_version": version_of(args.baseline, "version")} if args.baseline else {}),
            "machine": f"{platform.system()} {platform.machine()}",
            "versions": {
                "plum": version_of(args.plum, "version"),
                **{name: it["version"] for name, it in others.items()},
            },
            "seconds": results,
        }
        with open(args.json, "w") as f:
            json.dump(doc, f, indent=2)
            f.write("\n")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
