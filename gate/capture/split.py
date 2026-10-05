#!/usr/bin/env python3
"""Split raw/app.log into one SQL file per request, using the windows in raw/windows.json.
Output: raw/sql/<name>.txt with the SQLTRACE lines (start+end) that fall inside [start-0.05, end+0.9]."""
import json, re, datetime, os
R = "raw"
win = json.load(open(f"{R}/windows.json"))["windows"]
lines = []
for l in open(f"{R}/app.log", errors="replace"):
    l = re.sub(r"\x1b\[[0-9;]*m", "", l.rstrip("\n"))
    m = re.match(r"(\S+Z) (.*)", l)
    if not m: continue
    ts = datetime.datetime.fromisoformat(m.group(1)[:26] + "+00:00").timestamp()
    lines.append((ts, m.group(2)))
os.makedirs(f"{R}/sql", exist_ok=True)
for w in win:
    if w["name"].startswith("mark"): continue
    sel = [f"{t:.6f} {x}" for t, x in lines if w["start"] - 0.05 <= t <= w["end"] + 0.9 and x.startswith("SQLTRACE")]
    fn = re.sub(r"[^A-Za-z0-9]+", "-", w["name"]).strip("-").lower()
    open(f"{R}/sql/{fn}.txt", "w").write("\n".join(sel) + "\n")
    print(fn, len(sel))
