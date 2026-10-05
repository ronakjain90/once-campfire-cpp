#!/usr/bin/env python3
"""Build raw/sql-statements.json and the statement lists of summary.md from raw/sql/*.txt and raw/windows.json."""
import json, re, collections
R = "raw"
W = {w["name"]: w for w in json.load(open(f"{R}/windows.json"))["windows"]}

def parse(fn):
    out, stack = [], collections.defaultdict(list)
    for l in open(fn):
        m = re.match(r"(\S+) SQLTRACE (start|end) (\w+) ThreadId\((\d+)\) (.*)$", l.rstrip("\n"))
        if not m: continue
        ts, kind, role, th, rest = m.groups()
        if kind == "start":
            st = dict(t=float(ts), role=role, thread=int(th), sql=rest, rows=None, nested=rest.startswith("-- "))
            out.append(st); stack[th].append(st)
        else:
            rows, _ = re.match(r"rows=(\d+) (.*)$", rest).groups()
            if stack[th]: stack[th].pop()["rows"] = int(rows)
    return out

def norm(sql):
    s = re.sub(r"'[^']*'", "?", sql); s = re.sub(r"\b\d+\b", "?", s); return s

def table_of(sql):
    return sorted(set(re.findall(r'(?:FROM|JOIN|INTO|UPDATE)\s+"?([a-z_]+)"?', sql)))

res = {}
for name, fn in (("room_gzip_3", "get-room-gzip-3"), ("room_identity_3", "get-room-identity-3")) + tuple((f"post_{i}", f"post-message-{i}") for i in range(1, 12)):
    st = parse(f"{R}/sql/{fn}.txt")
    wname = {"room_gzip_3": "GET room gzip #3", "room_identity_3": "GET room identity #3"}.get(name, f"POST message #{name.split('_')[1]}" if name.startswith("post") else name)
    w = W[wname]
    for s in st: s["after_response"] = s["t"] > w["end"]
    res[name] = st
json.dump(res, open(f"{R}/sql-statements.json", "w"), indent=1)

# consistency over the 11 posts: multiset of normalized non-nested statements
sigs = {}
for i in range(1, 12):
    c = collections.Counter((s["role"], norm(s["sql"])) for s in res[f"post_{i}"] if not s["nested"])
    sigs[i] = c
base = sigs[3]
diff = {i: dict((base - c) + (c - base)) for i, c in sigs.items() if c != base}
print("posts with a different top-level statement multiset than post 3:", {i: len(d) for i, d in diff.items()})
for i, d in diff.items():
    for k, v in d.items(): print(i, v, k[0], k[1][:200])
for name in ("room_gzip_3", "room_identity_3", "post_3"):
    st = res[name]
    top = [s for s in st if not s["nested"]]
    print(name, "total", len(st), "top-level", len(top), collections.Counter(s["role"] for s in top), "nested", len(st) - len(top))
