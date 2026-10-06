# A small database for tools/local_sweep/sweep.py: one account, five users, five rooms. See README.md.
import sqlite3, sys, os, bcrypt
dest = sys.argv[1]
os.makedirs(os.path.join(dest, "db"), exist_ok=True)
path = os.path.join(dest, "db", "production.sqlite3")
if os.path.exists(path):
    os.remove(path)
db = sqlite3.connect(path)
db.executescript(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "spec", "schema.sql")).read())
T = "2026-09-26 13:00:20.000000"
digest = bcrypt.hashpw(b"secret123456", bcrypt.gensalt(4, prefix=b"2a")).decode()
db.execute("insert into accounts (name, join_code, singleton_guard, created_at, updated_at) values ('37signals','CRMu-l8Ge-KB9B',0,?,?)", (T, T))
users = [(1, "David", "david@example.com", 1), (2, "Jason Fried", "jason@example.com", 0), (3, "Kevin", "kevin@example.com", 0),
         (4, "Anna Lee", "anna@example.com", 0), (5, "Bender Bot", None, 2)]
for i, (id_, name, email, role) in enumerate(users):
    ts = "2026-09-26 13:00:%02d.000000" % (20 + i)
    db.execute("insert into users (id,name,email_address,password_digest,role,status,created_at,updated_at) values (?,?,?,?,?,0,?,?)",
               (id_, name, email, digest if email else None, role, ts, ts))
rooms = [(1, "All Pets", "Rooms::Open", 1), (2, "Designers", "Rooms::Closed", 1), (3, None, "Rooms::Direct", 1),
         (4, "HQ", "Rooms::Open", 2), (5, "Secret", "Rooms::Closed", 2)]
for id_, name, typ, creator in rooms:
    ts = "2026-09-26 13:01:%02d.000000" % id_
    db.execute("insert into rooms (id,name,type,creator_id,created_at,updated_at) values (?,?,?,?,?,?)", (id_, name, typ, creator, ts, ts))
members = {1: [1, 2, 3, 4], 2: [1, 3], 3: [1, 2], 4: [1, 2, 3, 4], 5: [2, 3]}
mid = 1
for room, ids in members.items():
    typ = [r for r in rooms if r[0] == room][0][2]
    inv = "everything" if typ == "Rooms::Direct" else "mentions"
    for u in ids:
        db.execute("insert into memberships (id,room_id,user_id,involvement,created_at,updated_at) values (?,?,?,?,?,?)", (mid, room, u, inv, T, T))
        mid += 1
for v in "20231215043540 20231220143106 20240110071740 20240115124901 20240130003150 20240130213001 20240131105830 20240209110503 20250825100957 20250825100958 20250825100959 20251126092013 20251126115722 20251126130131 20251212154340".split():
    db.execute("insert or ignore into schema_migrations (version) values (?)", (v,))
db.commit()
print("seeded", path)
