#!/usr/bin/env python3
"""Dump rows that the posts changed: rows of the post-capture DB that are new or different from the seed.
Usage (in runner): dump-tables.py SEED_DB POST_DB OUTDIR"""
import sqlite3, sys, os, json
seed, post, out = sys.argv[1:]
os.makedirs(out, exist_ok=True)
s = sqlite3.connect(f"file:{seed}?mode=ro", uri=True); p = sqlite3.connect(f"file:{post}?mode=ro", uri=True)
def rows(db, t, key):
    cur = db.execute(f'select * from "{t}"'); cols = [c[0] for c in cur.description]
    ki = [cols.index(k) for k in key]
    return cols, {tuple(r[i] for i in ki): r for r in cur}
res = {}
for t, key in (("messages", ["id"]), ("action_text_rich_texts", ["id"]), ("memberships", ["id"]), ("rooms", ["id"]), ("message_search_index_content", ["id"])):
    cols, a = rows(s, t, key); _, b = rows(p, t, key)
    new = [k for k in b if k not in a]; chg = [k for k in b if k in a and a[k] != b[k]]
    with open(f"{out}/{t}.txt", "w") as f:
        f.write("columns: " + ", ".join(cols) + "\n")
        for k in sorted(new): f.write("NEW     " + json.dumps(b[k]) + "\n")
        for k in sorted(chg):
            f.write("CHANGED " + json.dumps(b[k]) + "\n")
            f.write("  was   " + json.dumps(a[k]) + "\n")
            f.write("  cols  " + ", ".join(c for c, x, y in zip(cols, a[k], b[k]) if x != y) + "\n")
    res[t] = (len(new), len(chg))
# FTS: query through the virtual table for the new rowids
newids = [r[0] for r in p.execute("select id from messages where id not in (%s)" % ",".join(str(k[0]) for k in rows(s, 'messages', ['id'])[1]))] if False else None
mids = sorted(k[0] for k in rows(p, "messages", ["id"])[1] if k not in rows(s, "messages", ["id"])[1])
with open(f"{out}/message_search_index.txt", "w") as f:
    f.write("rows of the FTS5 table message_search_index (rowid, body) for the new messages\n")
    for r in p.execute("select rowid, body from message_search_index where rowid in (%s) order by rowid" % ",".join(map(str, mids))): f.write(json.dumps(r) + "\n")
# FTS shadow tables
with open(f"{out}/message_search_index-shadow-counts.txt", "w") as f:
    for t in ("content", "docsize", "data", "idx", "config"):
        n0 = s.execute(f"select count(*) from message_search_index_{t}").fetchone()[0]; n1 = p.execute(f"select count(*) from message_search_index_{t}").fetchone()[0]
        f.write(f"message_search_index_{t}: seed {n0} rows, after posts {n1} rows\n")
print(json.dumps(res), "new message ids:", len(mids))
