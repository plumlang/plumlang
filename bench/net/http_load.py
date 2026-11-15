#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
"""Load-test bench/net/http_server.plum: requests per second and latency.

Starts the server, then runs N client processes, each keeping one HTTP/1.1
connection alive and sending requests back to back for a few seconds.

Usage: python3 bench/net/http_load.py [./plum] [--clients 8] [--seconds 5]
"""
import argparse
import http.client
import multiprocessing
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def client(port, path, seconds, out):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    count, lat = 0, []
    end = time.perf_counter() + seconds
    while time.perf_counter() < end:
        start = time.perf_counter()
        conn.request("GET", path)
        resp = conn.getresponse()
        resp.read()
        if resp.status != 200:
            raise SystemExit(f"status {resp.status}")
        lat.append(time.perf_counter() - start)
        count += 1
    out.put((count, lat))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("plum", nargs="?", default="./plum")
    ap.add_argument("--clients", type=int, default=8)
    ap.add_argument("--seconds", type=float, default=5)
    ap.add_argument("--port", type=int, default=8089)
    args = ap.parse_args()
    server = subprocess.Popen([args.plum, "run", os.path.join(HERE, "http_server.plum"), str(args.port)],
                              stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                http.client.HTTPConnection("127.0.0.1", args.port, timeout=1).request("GET", "/hello/x")
                break
            except OSError:
                time.sleep(0.05)
        for path in ["/hello/ada", "/api/item/7"]:
            out = multiprocessing.Queue()
            procs = [multiprocessing.Process(target=client, args=(args.port, path, args.seconds, out))
                     for _ in range(args.clients)]
            for p in procs:
                p.start()
            results = [out.get() for _ in procs]
            for p in procs:
                p.join()
            total = sum(c for c, _ in results)
            lat = sorted(l for _, ls in results for l in ls)
            p50 = lat[len(lat) // 2] * 1000
            p99 = lat[int(len(lat) * 0.99)] * 1000
            print(f"{path:<14} {total / args.seconds:9.0f} req/s   p50 {p50:.2f} ms   p99 {p99:.2f} ms"
                  f"   ({args.clients} connections)")
    finally:
        server.terminate()
        server.wait()


if __name__ == "__main__":
    sys.exit(main())
