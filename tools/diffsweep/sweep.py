#!/usr/bin/env python3
"""Diff sweep main program. Runs inside the campfire-diffsweep container (see ./diffsweep)."""
import argparse
import glob
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "lib"))
import harness  # noqa: E402
from runner import Run  # noqa: E402


def load_lists(paths, area):
    files = []
    for p in paths:
        files += sorted(glob.glob(os.path.join(p, "*.json"))) if os.path.isdir(p) else [p]
    lists = []
    for f in files:
        d = json.load(open(f))
        d["_file"] = f
        if area and d.get("area") not in area:
            continue
        lists.append(d)
    return lists


def main():
    ap = argparse.ArgumentParser(description="Compare two Campfire app images on request lists.")
    ap.add_argument("lists", nargs="*", default=[os.path.join(HERE, "lists")], help="list files or directories")
    ap.add_argument("--expected", default="campfire-rust:app")
    ap.add_argument("--actual", default="campfire-cpp:app")
    ap.add_argument("--area", action="append", help="run only this area (A1..A9); repeatable")
    ap.add_argument("--only", help="run only scenarios whose name contains this")
    ap.add_argument("--seed", help="override the seed of every list")
    ap.add_argument("--port", type=int, default=4401, help="port of the expected app; actual uses port+1")
    ap.add_argument("--host-header", default="127.0.0.1:4390")
    ap.add_argument("--out", help="write the JSON report here")
    ap.add_argument("--keep-going-verbose", action="store_true", help="also print passing requests")
    a = ap.parse_args()

    lists = load_lists(a.lists, set(a.area or []))
    if not lists:
        sys.exit("no request lists selected")
    # Work items: (seed, mutates, list, scenario).
    items = []
    for d in lists:
        for s in d["scenarios"]:
            if a.only and a.only not in s["name"]:
                continue
            items.append((a.seed or s.get("seed") or d.get("seed", "default"), bool(s.get("mutates")), d, s))
    items.sort(key=lambda t: (t[0], t[1]))  # read-only scenarios first, one shared pair per seed
    tag = str(os.getpid())
    pair = harness.Pair(a.expected, a.actual, a.port, tag)
    results, t0, cur = [], time.time(), None
    try:
        run = None
        for seed, mutates, d, s in items:
            if cur != (seed, False) or mutates or pair.logins + sum(
                    1 for x in s["steps"] if x.get("op") == "login") > 8:
                harness.log(f"starting apps on seed {seed} ({a.expected} vs {a.actual})")
                pair.start(seed, d.get("env", []))
                run = Run(pair, harness.seed_labels(seed), a.host_header,
                          {"ignored": d.get("ignore_headers", []), "normalizers": d.get("normalize", [])})
                cur = (seed, not mutates and False)
                if mutates:
                    cur = None
            ctx = {"area": d.get("area", "?"), "list": os.path.basename(d["_file"])}
            n0 = len(run.results)
            res = run.run_scenario(ctx, s)
            bad = [r for r in res if not r["ok"]]
            harness.log(f"{ctx['area']} {s['name']}: {len(res)} checks, {len(bad)} differ")
            results += res
            if mutates:
                cur = None
    finally:
        pair.stop()

    by_area = {}
    for r in results:
        c = by_area.setdefault(r["area"], [0, 0])
        c[0] += 1
        c[1] += 0 if r["ok"] else 1
    for r in results:
        if r["ok"] and not a.keep_going_verbose:
            continue
        head = "PASS" if r["ok"] else "FAIL"
        print(f"{head} [{r['area']}] {r['scenario']} :: {r['name']}  {r['method']} {r['path']}  "
              f"status {r['status'][0]}/{r['status'][1]}" + (f"  ({', '.join(r['problems'])})" if r["problems"] else ""))
        if r["report"]:
            print("    " + r["report"].replace("\n", "\n    "))
    print("\nArea  requests  differences")
    for k in sorted(by_area):
        print(f"{k:<5} {by_area[k][0]:>8}  {by_area[k][1]:>11}")
    total, diff = sum(v[0] for v in by_area.values()), sum(v[1] for v in by_area.values())
    print(f"total {total:>8}  {diff:>11}   ({time.time() - t0:.0f}s, expected={a.expected}, actual={a.actual})")
    if a.out:
        json.dump({"expected": a.expected, "actual": a.actual, "results": results}, open(a.out, "w"), indent=1)
    sys.exit(1 if diff else 0)


if __name__ == "__main__":
    main()
