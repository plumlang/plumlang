#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
"""Time the benchmarks in bench/ against their Python versions.

Each program runs `--runs` times (default 3) and the fastest run counts.
Both versions must print the same output, or the benchmark is reported as
wrong rather than timed.

Usage: python3 bench/run.py [./plum] [--runs N] [--only NAME]
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def best_time(cmd, runs):
    best, out = None, None
    for _ in range(runs):
        start = time.perf_counter()
        done = subprocess.run(cmd, capture_output=True, text=True)
        elapsed = time.perf_counter() - start
        if done.returncode != 0:
            return None, done.stderr.strip()
        out = done.stdout
        best = elapsed if best is None else min(best, elapsed)
    return best, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("plum", nargs="?", default="./plum")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--only")
    args = ap.parse_args()
    python = shutil.which("python3") or sys.executable

    names = sorted(f[:-5] for f in os.listdir(HERE) if f.endswith(".plum"))
    if args.only:
        names = [n for n in names if n == args.only]
    print(f"{'benchmark':<12} {'plum':>9} {'python3':>9} {'speed-up':>9}")
    for name in names:
        p_time, p_out = best_time([args.plum, "run", os.path.join(HERE, name + ".plum")], args.runs)
        py_file = os.path.join(HERE, "python", name + ".py")
        q_time, q_out = best_time([python, py_file], args.runs) if os.path.exists(py_file) else (None, None)
        if p_time is None:
            print(f"{name:<12} failed: {p_out}")
            continue
        if q_out is not None and p_out != q_out:
            print(f"{name:<12} WRONG: plum printed {p_out!r}, python {q_out!r}")
            continue
        ratio = f"{q_time / p_time:8.2f}x" if q_time else "        -"
        q_text = f"{q_time:8.3f}s" if q_time else "        -"
        print(f"{name:<12} {p_time:8.3f}s {q_text} {ratio}")


if __name__ == "__main__":
    main()
