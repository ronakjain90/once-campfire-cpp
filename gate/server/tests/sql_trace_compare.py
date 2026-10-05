#!/usr/bin/env python3
"""Compare the gate's SQL trace with the G3 list of the Rust app (the `?` forms of each statement)."""
import re, sys
raw, which, gate_file = sys.argv[1], sys.argv[2], sys.argv[3]
import json
key = {'room': 'room_gzip_3', 'post': 'post_3'}[which]
END = re.compile(r'SQLTRACE end (\w+) ThreadId\(\d+\) rows=(\d+) (.*)$')
LIT = re.compile(r"'[^']*'|\?\d*|\b\d+\b")
def norm(sql): return LIT.sub('?', sql)

def parse(path):
    out = []
    for l in open(path, errors='replace'):
        m = END.search(l.rstrip('\n'))
        if m: out.append((m.group(1), int(m.group(2)), m.group(3)))
    return out

# Rust list in the order of summary.md (statement start order), literals normalized
rust = [(e['role'], e['rows'], norm(e['sql'])) for e in json.load(open(f'{raw}/sql-statements.json'))[key] if not e['nested']]
gate_all = parse(gate_file)
isx = lambda s: s.startswith(('SAVEPOINT', 'RELEASE', 'ROLLBACK'))
gate = [(r, n_, norm(s)) for (r, n_, s) in gate_all if not isx(s)]
extra = [s for (_, _, s) in gate_all if isx(s)]
skipped = []
exp = list(rust)
for prefix in ('SELECT "sessions"', 'SELECT "users"'):
    for i, x in enumerate(exp):
        if x[2].startswith(prefix):
            skipped.append(exp.pop(i)); break

def short(s): return (s[:92] + '...') if len(s) > 95 else s
print(f'=== {which}: Rust statements (G3) | gate statements (GATE_SQL_TRACE)')
n = max(len(rust), len(gate))
gi = 0
for i, x in enumerate(rust):
    note = ''
    if True:
        # a skipped statement: shown with no gate counterpart
        if any(x is k for k in skipped):
            print(f'{i+1:2d} {x[0]:6s} rows={x[1]:<2d} {short(x[2])}\n   -- not run by the gate (session cache)')
            continue
    g = gate[gi] if gi < len(gate) else None
    gi += 1
    same = g is not None and g[2] == x[2] and g[1] == x[1] and g[0] == x[0]
    print(f'{i+1:2d} {x[0]:6s} rows={x[1]:<2d} {short(x[2])}\n   gate: {"SAME " if same else "DIFF "}{g[0] + " rows=" + str(g[1]) if g else "(missing)"}')
ok = [(r, n_, s) for (r, n_, s) in gate] == [(r, n_, s) for (r, n_, s) in exp]
if extra: print('gate-only statements (group commit):', ', '.join(sorted(set(extra))))
print(f'RESULT {which}: gate list {"EQUALS" if ok else "DIFFERS FROM"} the Rust list minus the session and user reads '
      f'({len(gate)} gate statements, {len(exp)} expected)')
if not ok:
    for i in range(max(len(gate), len(exp))):
        a = exp[i] if i < len(exp) else None; b = gate[i] if i < len(gate) else None
        if a != b: print(' first difference at', i + 1, '\n  rust:', a, '\n  gate:', b); break
sys.exit(0 if ok else 1)
