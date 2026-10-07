#!/usr/bin/env python3
"""Generates the request lists in lists/ (run on the host: `diffsweep gen`).

Sources:
  parity/screens.yml                          -> lists/screens-A*.json  (the GET of every state)
  reference-tools/http_shape/sweep.py         -> lists/shape-A*.json    (its request set)
  gen_writes.py                               -> lists/writes-A*.json, cable-A7.json, front-A8.json, jobs-A9.json
Edit the generators, not the JSON.
"""
import json
import os
import re
import sys

import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
WORKSPACE = os.environ.get("CAMPFIRE_WORKSPACE") or os.path.abspath(os.path.join(HERE, "..", "..", ".."))
RUST = os.path.join(WORKSPACE, "once-campfire-rust")
SEED_ROOT = f"{RUST}/parity/.seed"
OUT = os.path.join(HERE, "lists")
AREAS = ["A1", "A2", "A3", "A4", "A5", "A6", "A7", "A8", "A9"]
CHROME = ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
          "Chrome/140.0.0.0 Safari/537.36")


def labels(seed):
    return json.load(open(f"{SEED_ROOT}/{seed}/labels.json"))


def area_of(path):
    """Maps a request path to its sweep area (A1 to A9)."""
    p = path.split("?")[0]
    if re.match(r"^/(session|first_run|join|users/new)", p) or p == "/":
        return "A1"
    if re.match(r"^/rooms/[^/]+/[^/]+/messages", p) or "/messages" in p or p.startswith(("/messages", "/autocompletable")):
        return "A3"
    if p.startswith("/rooms") or p.startswith("/users/me/sidebar"):
        return "A2"
    if p.startswith("/searches"):
        return "A5"
    if p.startswith(("/account", "/users", "/qr_code")):
        return "A4"
    return "A6"  # pwa, /up, error pages, Active Storage, unfurl, assets


def resolve_ua(name, uas):
    return uas.get(name, name)


def referenced(obj):
    return set(re.findall(r"\{\{\s*([^}|\s]+)", json.dumps(obj)))


def screens_lists():
    states = yaml.safe_load(open(f"{RUST}/parity/screens.yml"))
    uas = yaml.safe_load(open(f"{RUST}/parity/seeds/user_agents.yml"))
    groups = {}   # (area, seed, mutates, actor) -> steps
    seen, skipped = set(), []
    for s in states:
        seed = s.get("seed", "default")
        actor = s.get("as", "anon")
        req = s.get("request")
        step = {"path": s["path"], "name": s["id"]}
        if actor != "anon":
            step["actor"] = actor
        if s.get("user_agent"):
            step["user_agent"] = resolve_ua(s["user_agent"], uas)
        hdrs = dict(s.get("headers", {}))
        if req:
            step["method"] = req.get("method", "GET")
            hdrs.update(req.get("headers", {}))
            if "body" in req:
                step["body"] = req["body"]
            if actor == "anon":
                step["csrf"] = False
        if hdrs:
            step["headers"] = hdrs
        if s.get("kind") == "fragment" and not req:
            step["revisit"] = True
        key = json.dumps(step, sort_keys=True).replace(json.dumps(s["id"]), "")
        if key in seen:
            continue
        seen.add(key)
        missing = [k for k in referenced(step) if k not in labels(seed)]
        if missing:
            skipped.append((s["id"], missing))
            continue
        area = area_of(s["path"])
        gk = (area, seed, bool(req), actor)
        groups.setdefault(gk, []).append(step)
    lists = {}
    for (area, seed, mut, actor), steps in sorted(groups.items()):
        name = f"screens {seed} {actor}" + (" requests" if mut else "")
        sc = {"name": name, "seed": seed, "steps": ([{"op": "login", "actor": actor}] if actor != "anon" else []) + steps}
        if mut:
            sc["mutates"] = True
        lists.setdefault(area, []).append(sc)
    for sid, miss in skipped:
        print(f"skipped screens state {sid}: undefined labels {miss}", file=sys.stderr)
    return lists


def write(area, kind, scenarios, extra=None):
    os.makedirs(OUT, exist_ok=True)
    doc = {"area": area, "source": kind, "scenarios": scenarios}
    doc.update(extra or {})
    path = os.path.join(OUT, f"{kind}-{area}.json")
    with open(path, "w") as f:
        f.write("{\n" + ",\n".join(f' "{k}": ' + json.dumps(v) if k != "scenarios" else
                                   ' "scenarios": [\n' + ",\n".join("  " + json.dumps(s) for s in v) + "\n ]"
                                   for k, v in doc.items()) + "\n}\n")
    n = sum(len(s["steps"]) for s in scenarios)
    print(f"{path}: {len(scenarios)} scenarios, {n} steps")


def main():
    for f in os.listdir(OUT) if os.path.isdir(OUT) else []:
        if f.endswith(".json") and f != "smoke.json":
            os.remove(os.path.join(OUT, f))
    for area, scs in sorted(screens_lists().items()):
        write(area, "screens", scs)
    import gen_shape
    import gen_writes
    gen_shape.generate(write)
    gen_writes.generate(write)


if __name__ == "__main__":
    main()
