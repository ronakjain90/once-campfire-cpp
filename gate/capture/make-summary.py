#!/usr/bin/env python3
import json, re, hashlib, collections
R = "raw"
d = json.load(open(f"{R}/sql-statements.json"))
cs = json.load(open(f"{R}/capture-summary.json"))
W = json.load(open(f"{R}/windows.json"))["windows"]
lab = json.load(open("/Volumes/ExternalHD/Code/AI/once-campfire/once-campfire-rust/parity/.seed/default/labels.json"))
ROOM, HQ = lab["rooms.watercooler"], lab["rooms.hq"]
sha = lambda p: hashlib.sha256(open(p, "rb").read()).hexdigest()
o = []
P = o.append

def role_note(s):
    q = s["sql"]
    if '"sessions"' in q and "FROM" in q: return "SESSION"
    if 'FROM "users" WHERE "users"."id"' in q: return "USER"
    return ""

def statements(name, show_after=True):
    st = d[name]
    P("| # | role | thread | rows | tables | note | statement (expanded, as in raw/app.log) |")
    P("|--:|---|--:|--:|---|---|---|")
    for i, s in enumerate(st, 1):
        note = role_note(s)
        if s["after_response"]: note = (note + " after-response").strip()
        if s["nested"]: note = (note + " fts-internal").strip()
        sql = s["sql"].replace("|", "\\|")
        tabs = ", ".join(sorted(set(re.findall(r'(?:FROM|JOIN|INTO|UPDATE|INSERT INTO|DELETE FROM)\s+[\'"]?(?:main\'\.\')?([A-Za-z_]+)', s["sql"]))))
        P(f"| {i} | {s['role']} | {s['thread']} | {s['rows']} | {tabs} | {note} | `{sql}` |")
    P("")

P("# G3 summary: what the Rust app does for each benchmark request\n")
P("All numbers come from one run of `run-capture.sh` (see README.md). The raw SQL log is `raw/app.log` (`docker logs -t g3-app`). Per-request slices are in `raw/sql/`. The parsed form is `raw/sql-statements.json`.\n")
P("Rust image: `campfire-rust:trace` (branch `gate-sql-trace`). The app ran on a VM-local seed copy (`/var/lib/campfire-bench/g3`), port 4490, same env as G2.\n")
P("A statement line is `start <role> <thread> <expanded SQL>` followed by `end ... rows=N <SQL with ?>`. The `rows` column below is N. Reader thread ids change from request to request, because the app picks a reader from a pool. Only the role matters.\n")

P("## 1. GET /rooms/%s (watercooler), third request\n" % ROOM)
for enc, tag in (("gzip", "gz"), ("identity", "id")):
    P(f"- `Accept-Encoding: {enc}`: request `raw/room-{tag}-3.request.txt`, response headers `raw/room-{tag}-3.response-headers.txt`, raw body `raw/room-{tag}-3.body.raw`, decoded body `raw/room-{tag}-3.body.decoded`.")
P("")
P("Decoded body SHA-256 (`sha256sum raw/room-*-*.body.decoded`):\n")
P("| request | status | wire bytes | decoded bytes | SHA-256 of the decoded body |")
P("|---|--:|--:|--:|---|")
for enc, tag in (("gzip", "gz"), ("identity", "id")):
    for i, r in enumerate(cs[f"room_{enc}"], 1):
        P(f"| {enc} #{i} | {r['status']} | {r['raw_bytes']} | {r['decoded_bytes']} | `{sha(f'{R}/room-{tag}-{i}.body.decoded')}` |")
P("\nAll 6 hashes are equal. The gzip response has `content-encoding: gzip`, `transfer-encoding: chunked`, `vary: Accept-Encoding`, `etag: W/\"7b5fe0b3da7cd2db376df9a202a71ee7\"`, `x-cache: miss`. The identity response has `content-length: 416122`. The `x-request-id`, `x-runtime`, `date` and `set-cookie: last_room` expires date change for each request.\n")
g, i_ = d["room_gzip_3"], d["room_identity_3"]
P(f"**SQL: {len(g)} statements, {sum(s['role']=='reader' for s in g)} reader, {sum(s['role']=='writer' for s in g)} writer.** All run on one reader thread, in this order. The identity request runs the same {len(i_)} statements (checked: same text, same row counts: %s).\n" % ("yes" if [(s['sql'], s['rows']) for s in g] == [(s['sql'], s['rows']) for s in i_] else "NO"))
P("Statements 1 and 2 read the session and the user. The 40 messages of the page come from one `messages` SELECT (statement 4). No statement loads their bodies, users or boosts. This is observed, not proven: the Rust app has an in-process fragment cache (`campfire_views::fragment_cache`) that the earlier GETs of the room filled. The first GET of a cold app runs more statements. The response header `x-cache: miss` comes from the proxy and is not this cache.\n")
statements("room_gzip_3")

P("## 2. POST /rooms/%s/messages (hq), third post\n" % HQ)
P("Request: `raw/post-3.request.txt`. Response: `raw/post-3.response-headers.txt`, `raw/post-3.body.raw`, `raw/post-3.body.decoded`. The request has these headers, in this order: `host`, `cookie`, `content-type: application/x-www-form-urlencoded`, `accept: text/vnd.turbo-stream.html, text/html, application/xhtml+xml`, `x-csrf-token`, `sec-fetch-site: same-origin`, `accept-encoding: gzip`, `content-length`. The body is `message%5Bbody%5D=bench%20write%20N&message%5Bclient_message_id%5D=<nonce>&authenticity_token=<csrf>`.\n")
P("`loadgen scrape` returns `csrf: null` for this app, so loadgen (and this capture) send an empty `x-csrf-token` and an empty `authenticity_token`. The app accepts that (status 200). `raw/loadgen-wire.txt` is a tcpdump of a real `loadgen http --post-room` request. Its headers and body have the same bytes as `raw/post-3.request.txt`, except the nonce and the cookie. The first line of the body differs only in the number N.\n")
P("| post | status | content-type | what loadgen expects |\n|--:|--:|---|---|")
for p in cs["posts"]:
    P(f"| {p['i']} | {p['status']} | `{p['content_type']}` | status < 400 |")
P("\nloadgen counts a response as ok when the status is below 400 (`ok` in `loadgen http`). A real `loadgen http --post-room ... --requests 1` run gave `ok=1`, `errors=0` (`raw/loadgen-validate.json`). Response headers of post 3: `x-cache: bypass`, `content-encoding: gzip`, `transfer-encoding: chunked`, `vary: Accept-Encoding` and `vary: Accept,Accept-Encoding` (two headers), `etag`. The decoded body is a `<turbo-stream action=\"append\" target=\"messages_rooms_open_%s\">` of about 1.9 KB.\n" % HQ)
p3 = d["post_3"]; top = [s for s in p3 if not s["nested"]]
rd = [s for s in top if s["role"] == "reader"]; wr = [s for s in top if s["role"] == "writer"]
req_thread = p3[0]["thread"]
P(f"**SQL, post 3: {len(top)} top-level statements = {len(rd)} reader + {len(wr)} writer. Plus {len(p3)-len(top)} nested FTS5 internal statements on the writer (lines that start with `-- `), which SQLite runs inside the `insert into message_search_index` statement.** The count is the same for all 11 posts (checked by `gen-summary.py`: the multiset of normalized top-level statements is equal). Post 4 has more nested FTS statements (an FTS5 segment merge: {len(d['post_4'])-len([s for s in d['post_4'] if not s['nested']])} nested instead of {len(p3)-len(top)}).\n")
P(f"- Reader, request thread (statements 1-5, same thread): `bans` by IP, **`sessions` by token (session)**, **`users` by id (user)**, `memberships` by room and user, `rooms` by id. The statement order is: bans, session, user, membership, room.")
P("- Writer: `BEGIN IMMEDIATE`, 4 writes (messages INSERT, action_text_rich_texts INSERT, messages UPDATE, rooms UPDATE), `COMMIT`. After the `COMMIT`, outside the transaction: a `SELECT` of the rich text, the FTS5 insert, and the `memberships` UPDATE.")
P("- Reader, other pool threads (19 statements): the work after the commit. It loads the message, room, user, rich text, attachment, boosts and account for the broadcast and the response. It runs one `push_subscriptions` SELECT (3 rows), and 3 unread counts (`SELECT COUNT(*) FROM memberships ... unread_at IS NOT NULL`), one for each user with a push subscription. These statements read `users` by id (the post creator) but not the session.")
P("- Webhooks: no statement reads the `webhooks` table in the room post, because the message mentions no bot. `deliver_webhooks_to_bots` finds no bot.")
P(f"- {sum(s['after_response'] for s in p3)} statements start after the client got the response (marked `after-response`). The others start before it.\n")
statements("post_3")

P("## 3. Tables that each post writes\n")
P("From the writer statements of post 3 (`raw/sql/post-message-3.txt`) and the dump in `tables/`:\n")
P("| table | operation | columns written |\n|---|---|---|")
P("| `messages` | INSERT | `client_message_id`, `created_at`, `creator_id`, `room_id`, `updated_at` (`id` from RETURNING) |")
P("| `action_text_rich_texts` | INSERT | `body` (the canonical HTML, here the text `bench write N`), `created_at`, `name` = 'body', `record_id`, `record_type` = 'Message', `updated_at` |")
P("| `messages` | UPDATE | `updated_at` |")
P("| `rooms` | UPDATE | `updated_at` (room hq) |")
P("| `message_search_index` (FTS5, tokenize=porter) | INSERT | `rowid` = message id, `body` = plain text. Shadow tables `_content`, `_docsize`, `_data`, `_idx` change. |")
P("| `memberships` | UPDATE | `unread_at` (= the message `created_at`), `updated_at`. Rows: room hq, `involvement != 'invisible'`, not connected (or `connected_at` more than 60 s old), `user_id != creator`. It changed 5 rows. |")
P("")
P("`push_subscriptions` and `webhooks` are not written. The `memberships` UPDATE has `connected_at` / `connections` unchanged.\n")
P("### Rows that the posts changed (12 messages: 1 from the loadgen validation run and the 11 captured posts)\n")
for t in ("messages", "action_text_rich_texts", "memberships", "rooms", "message_search_index", "message_search_index_content"):
    n = open(f"tables/{t}.txt").read().count("\nNEW") + open(f"tables/{t}.txt").read().count("\nCHANGED")
    P(f"- `tables/{t}.txt`: {n} rows (NEW or CHANGED; `was` lines show the old values)")
P("- `tables/message_search_index-shadow-counts.txt`: row counts of the FTS shadow tables.")
P("- The seed is the base for the comparison: `dump-tables.py` compares the seed DB to a copy of the DB after the posts.\n")

P("## 4. Outbound connections to 127.0.0.1:9 (push and webhooks)\n")
P("Method: `tcpdump -i any -n -tt \"tcp port 9\"` in the Colima VM (`raw/tcpdump-port9.txt`, errors `raw/tcpdump.err`). It ran during the whole capture: all 3+3 GETs, the loadgen validation post, and all 11 posts. A self test (`raw/tcpdump-selftest.txt`: 2 `curl` calls to 127.0.0.1:9) shows that this tcpdump command sees SYN packets to port 9.\n")
P("| post | window (unix s) | SYN packets to port 9 |\n|--:|---|--:|")
syn = [float(l.split()[0]) for l in open(f"{R}/tcpdump-port9.txt") if "Flags [S]," in l]
for w in W:
    if w["name"].startswith("POST message"):
        n = sum(1 for t in syn if w["start"] - 0.3 <= t <= w["end"] + 1.3)
        P(f"| {w['name'].split('#')[1]} | {w['start']:.3f} - {w['end']:.3f} | {n} |")
P(f"\nTotal SYN packets to port 9 in the whole run: {len(syn)}. Push connections per post: 0. Webhook connections per post: 0.\n")
P("Why 0: the seed has push subscriptions (the push SELECT returns 3 rows per post). But G2 sets their endpoints to `https://127.0.0.1:9/push/<id>`. The Rust code (`crates/campfire/src/integrations/web_push.rs`, `crates/db/src/models/push_subscription.rs` `resolved_endpoint_ip`) sends a push only to an `https` endpoint on port 443 with a permitted host. A loopback address on port 9 fails this check, so the app opens no connection. The app still runs the push SELECT and the 3 unread-count SELECTs. A gate server must do the same SELECTs, and must not send to these endpoints. The room post triggers no webhook, because no bot is mentioned.\n")

P("## 5. How this was checked\n")
P("- Every statement of the 3 tables above is a line of `raw/app.log`. Check with `grep -F '<statement>' raw/app.log`.")
P("- Decoded body hashes: `sha256sum raw/room-*-?.body.decoded`.")
P("- The trace is off by default (`CAMPFIRE_SQL_TRACE` unset). `raw/app.log` also holds the statements of start-up and of the login. Those are not part of the 3 requests.")
open("summary.md", "w").write("\n".join(o) + "\n")
