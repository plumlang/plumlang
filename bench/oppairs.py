#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
"""Which instructions, and pairs of them, the benchmarks run (#123).

Needs a build that counts them, which normal builds don't:

    make DEFS=-DPLUM_OP_COUNTS=1 BUILD=build/opcounts
    python3 bench/oppairs.py build/opcounts/plum [--top 15] [--triples]

Runs every bench/*.plum and examples/*.plum with PLUM_OP_COUNTS set, then
prints, as Markdown tables, the pairs of instructions run one after the
other where the second is the next one in the code (the ones a
superinstruction could join), weighted by how often they ran: their
share of all the instructions run, and the mean of their share in each
program, so that a long benchmark doesn't decide alone.
"""
import argparse
import collections
import glob
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def collect(plum, files):
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "counts.txt")
        env = dict(os.environ, PLUM_OP_COUNTS=out)
        for f in files:
            subprocess.run([plum, "run", f], env=env, stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        runs = {}
        cur = None
        with open(out) as fh:
            for line in fh:
                parts = line.split()
                if parts[0] == "R":
                    cur = os.path.relpath(parts[1], ROOT)
                    runs[cur] = {"S": collections.Counter(), "P": collections.Counter(),
                                 "J": collections.Counter(), "T": collections.Counter()}
                else:
                    runs[cur][parts[0]][tuple(parts[1:-1])] += int(parts[-1])
        return runs


def table(runs, kind, top):
    total = collections.Counter()
    share = collections.Counter()
    best = collections.defaultdict(list)
    all_ops = sum(sum(r["S"].values()) for r in runs.values())
    for name, r in runs.items():
        n = sum(r["S"].values())
        for key, c in r[kind].items():
            total[key] += c
            share[key] += c / n / len(runs)
            best[key].append((c / n, os.path.basename(name).rsplit(".", 1)[0]))
    rows = []
    for key, c in sorted(total.items(), key=lambda kv: -kv[1])[:top]:
        where = ", ".join(f"{b} {s:.0%}" for s, b in sorted(best[key], reverse=True)[:3]
                          if s >= 0.005)
        rows.append(f"| {' '.join(f'`{k}`' for k in key)} | {c / 1e6:.1f} M | "
                    f"{c / all_ops:.1%} | {share[key]:.1%} | {where} |")
    return rows, all_ops


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("plum")
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--triples", action="store_true")
    args = ap.parse_args()
    files = sorted(glob.glob(os.path.join(HERE, "*.plum"))) + \
        sorted(glob.glob(os.path.join(ROOT, "examples", "*.plum")))
    runs = collect(args.plum, files)
    kinds = [("S", "Instruction"), ("P", "Pair (in code order)"), ("J", "Pair (jumped)")]
    if args.triples:
        kinds.append(("T", "Triple"))
    for kind, title in kinds:
        rows, all_ops = table(runs, kind, args.top)
        print(f"| {title} | run | of all | mean share | most in |")
        print("|---|---|---|---|---|")
        print("\n".join(rows))
        print()
    print(f"{len(runs)} programs, {all_ops / 1e6:.0f} M instructions run", file=sys.stderr)


if __name__ == "__main__":
    main()
