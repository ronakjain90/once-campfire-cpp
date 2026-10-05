# G4: the gate server

A throwaway C++23 server for the go/no-go gate. It serves `GET /rooms/:id`, `POST /rooms/:id/messages`
and `GET /up`. It does the same per-request work as the Rust app, except where this file says it does not.

## Build and run

    docker build -t campfire-gate:app gate/server
    gate/server/tests/cookie_vector.sh      # the cookie vector test
    gate/server/tests/compare.sh            # bodies, header names, tables, against campfire-rust:app
    gate/server/tests/sql_trace.sh          # statement lists, against the G3 capture
    gate/bench/run --apps rust=campfire-rust:app,gate=campfire-gate:app --reps 1

The tests need bash 4 (`/opt/homebrew/bin/bash`), the images `campfire-rust:app` and `campfire-bench-runner`,
and ports 4590 and 4690.

Build flags: `-O2 -flto -march=armv8.2-a+crypto`, linked with lld, nothing else. SQLite is the 3.53.2
amalgamation that `rusqlite` 0.40 bundles (`libsqlite3-sys` 0.38.2), with the same compile options. Other
libraries: OpenSSL 3, libdeflate, picohttpparser (vendored), jemalloc, libcrypt (bcrypt, for sign-in).

## Design

- One worker thread for each CPU in the affinity mask. Each has an `epoll` loop, an `SO_REUSEPORT` listener,
  a read connection with prepared statements, and a per-worker map from the raw `session_token` cookie value
  to the user id. HTTP/1.1 keep-alive only. picohttpparser parses all headers. One `writev` for each response.
- Pragmas as `schema::configure_connection` of the Rust app: `busy_timeout=5000`, `foreign_keys`, `journal_mode=wal`,
  `synchronous=normal`, `journal_size_limit=67108864`, `cache_size=2000`; `query_only` on readers.
- One writer thread owns the write connection. It takes up to 64 queued writes into one `BEGIN IMMEDIATE`,
  each in its own `SAVEPOINT`. After `COMMIT` it runs, for each write in order, the three after-commit statements
  (rich text SELECT, FTS insert, memberships UPDATE; each autocommits, as in Rust). Then it answers each
  worker through a queue and an `eventfd`. The workers then do the 19 read statements on their own connections.
- The writer replaces the automatic checkpoint with a WAL hook (as Rust does). A checkpointer thread runs
  `wal_checkpoint(PASSIVE)` each time the WAL grows by 1,000 pages. At 10,000 pages the writer runs `RESTART`
  (Rust: `WAL_LIMIT_PAGES`).
- `GATE_SQL_TRACE=1` prints every statement (`SQLTRACE start|end <role> ThreadId(n) ...`, same format as the G3 trace).
  `GATE_WORKERS=n` overrides the worker count.

## The room page cache key

The key is computed on each request from the rows of the page statements (statements 3 to 9 of the G3 list):

| Part | Source | Why |
|---|---|---|
| user id | the session cache | the page shows the current user |
| every column of the room row | statement 3 | name, type, `updated_at` appear in the page |
| every column of the 40 message rows | statement 4 | id, creator, client id, `created_at`, `updated_at`. Rust keys each message fragment by id and `updated_at` |
| the id of the first (original) room | statement 5 | the page only compares it with the room id. Its other columns are left out on purpose: a post into another room changes `updated_at` of that room, which must not change this page |
| every column of the account row (twice) | statements 6, 7 | name, join code, styles, settings |
| every column of the logo attachment rows | statement 8 | logo link |
| every column of the room row again | statement 9 | |
| the `Host` header and the `User-Agent` header | request | the page text has the host (5 places) and the browser name from the user agent (2 places) |

The key is the byte string itself (no hash of the body). The ETag is `W/"<md5 of the key>"`, computed once when the
entry is made. A post into a different room changes none of these inputs, so the page stays cached. A post into
the shown room changes the message rows and the room `updated_at`, so the key changes.

Startup: the captured body (`fixtures/room_page.html`, G3 request 3) goes into the cache under the key that the
startup queries give for the captured user and room, with the captured host (`127.0.0.1:4490`) and no `User-Agent`.
If a request has the same database part of the key but another Host or User-Agent (the benchmark uses
`127.0.0.1:4390`), the gate fills the host and browser-name slots of the captured page, compresses it and caches
it under the new key (like Rust does for a cold page). This fill runs once for each different (Host, User-Agent).
The browser name comes from a small rule (blank gives `Mozilla`; `name/version` gives the capitalized name;
curl gives `Curl`). Any other User-Agent gets a 500 from the gate.

Both a gzip body (libdeflate level 6, already framed as one chunk) and the identity body are kept for each key.

## What the gate does NOT do (compared to the Rust app)

1. **Page rendering.** The gate cannot render a room page. If the database inputs differ from the captured page
   (any new message in the room, a changed room or account), it returns 500. Rust renders the page text on every
   request and uses its fragment cache only for the message fragments; the gate caches the whole page by key. This
   is the design difference that the project wants to measure. Only room 486777696 (watercooler) of user David
   has a body.
2. **Gzip.** Rust splices per-fragment gzip pieces; the gate compresses the whole page once with libdeflate
   level 6. The compressed size differs (about 20.7 KB against 24.2 KB on the wire). The decoded bodies are identical.
3. **ETag** of the room page is the MD5 of the key, not the digest of fragment digests. The value differs, the form is the same.
4. **Session and user reads** are skipped on a session-cache hit (allowed by the task). The cache is never invalidated;
   the gate has no way to change a session or a user. The hourly refresh of `sessions.last_active_at` (and the re-signed
   cookie) that Rust does for a session older than one hour is not done. The harness signs in just before it measures.
5. **Message body.** The gate treats the body as plain text and HTML-escapes it. Rust canonicalizes the body as HTML
   (parser, sanitizer, autolink, mentions). For `bench write N` the result is the same; the CPU work of the Rust
   canonicalizer and of the plain-text conversion (which is a tag strip plus entity decode here) is not done.
6. **Message HTML.** It comes from a template taken from the captured response (`fixtures/post_template.txt`, made by
   `tools/gen_post_template.py`) with slots for client id, message id, times, user id, name, avatar URL, room id,
   room name, room type and body. It does not render boosts or attachments (the statements run; their rows are empty for a new message).
   The avatar URL is signed as Rails does (`signed_id`, tested). The response is the turbo-stream the Rust response has.
7. **Broadcast.** The statements of the broadcast run (account, memberships of the room, rich text), the message
   fragment is put in a bounded (32 MB) fragment store, and a small JSON string is built for each member. There is no Action Cable
   hub, no WebSocket, no frame encoding. The benchmark has no subscribers. Rust's cost for a broadcast to nobody was not measured.
8. **Push.** The SELECT, the unread count for each subscription and the endpoint check (port of `PushSubscription`
   `resolved_endpoint_ip`) run. The benchmark endpoints fail the check, so nothing is encrypted or sent, as in Rust.
   A deliverable endpoint would be counted but not sent (not implemented). Webhooks: none, as in the capture.
9. **Request checks.** CSRF: the gate accepts `Sec-Fetch-Site: same-origin` and returns 422 otherwise (Rails also accepts a valid token; not implemented).
   A bad cookie gives a 302 to `/session/new` without the flash/session cookie that Rails sets. A room that the user cannot see gives a 302 to `/`
   with no alert. No rate limit on sign-in. Other routes give 404. A room that is the first room of the account (the `invitation` case) is not supported.
10. **Sign-in and scrape (not measured).** `POST /session` verifies email and bcrypt password (libcrypt), inserts a
   `sessions` row through the writer, and sets the `session_token` cookie as Rails does (tested against the vectors).
   `GET /session/new` is a stub page. `GET /users/me/sidebar` returns a body captured from the Rust app
   (`fixtures/sidebar.html`); it is not a real render.
11. `x-request-id` comes from a per-thread PRNG, not from `getrandom` for each request. `x-runtime` is the real time of the handler.
12. SQL text: the statements are the `?` forms of the Rust statements (generated from the capture by
    `tools/gen_sql.py`). The gate adds `SAVEPOINT gate_write` and `RELEASE gate_write` around each write.
    Result rows are read into an arena (every column of every row is read) and are not parsed into typed structs.

## Statement lists: Rust (G3) next to the gate (output of tests/sql_trace.sh)

```
=== room: Rust statements (G3) | gate statements (GATE_SQL_TRACE)
 1 reader rows=1  SELECT "sessions"."id", "sessions"."user_id", "sessions"."token", "sessions"."ip_address", "...
   -- not run by the gate (session cache)
 2 reader rows=1  SELECT "users"."id", "users"."name", "users"."email_address", "users"."password_digest", "us...
   -- not run by the gate (session cache)
 3 reader rows=1  SELECT "rooms"."id", "rooms"."name", "rooms"."type", "rooms"."creator_id", "rooms"."created_...
   gate: SAME reader rows=1
 4 reader rows=40 SELECT "messages"."id", "messages"."room_id", "messages"."creator_id", "messages"."client_me...
   gate: SAME reader rows=40
 5 reader rows=1  SELECT "rooms"."id", "rooms"."name", "rooms"."type", "rooms"."creator_id", "rooms"."created_...
   gate: SAME reader rows=1
 6 reader rows=1  SELECT "accounts"."id", "accounts"."name", "accounts"."join_code", "accounts"."custom_styles...
   gate: SAME reader rows=1
 7 reader rows=1  SELECT "accounts"."id", "accounts"."name", "accounts"."join_code", "accounts"."custom_styles...
   gate: SAME reader rows=1
 8 reader rows=0  SELECT b.id, b.key, b.filename, b.content_type, b.metadata, b.service_name, b.byte_size, b.c...
   gate: SAME reader rows=0
 9 reader rows=1  SELECT "rooms"."id", "rooms"."name", "rooms"."type", "rooms"."creator_id", "rooms"."created_...
   gate: SAME reader rows=1
RESULT room: gate list EQUALS the Rust list minus the session and user reads (7 gate statements, 7 expected)
=== post: Rust statements (G3) | gate statements (GATE_SQL_TRACE)
 1 reader rows=0  SELECT ? AS one FROM "bans" WHERE "bans"."ip_address" = ? LIMIT ?
   gate: SAME reader rows=0
 2 reader rows=1  SELECT "sessions"."id", "sessions"."user_id", "sessions"."token", "sessions"."ip_address", "...
   -- not run by the gate (session cache)
 3 reader rows=1  SELECT "users"."id", "users"."name", "users"."email_address", "users"."password_digest", "us...
   -- not run by the gate (session cache)
 4 reader rows=1  SELECT "memberships"."id", "memberships"."room_id", "memberships"."user_id", "memberships"."...
   gate: SAME reader rows=1
 5 reader rows=1  SELECT "rooms"."id", "rooms"."name", "rooms"."type", "rooms"."creator_id", "rooms"."created_...
   gate: SAME reader rows=1
 6 writer rows=0  BEGIN IMMEDIATE
   gate: SAME writer rows=0
 7 writer rows=1  INSERT INTO "messages" ("client_message_id", "created_at", "creator_id", "room_id", "updated...
   gate: SAME writer rows=1
 8 writer rows=1  INSERT INTO "action_text_rich_texts" ("body", "created_at", "name", "record_id", "record_typ...
   gate: SAME writer rows=1
 9 writer rows=0  UPDATE "messages" SET "updated_at" = ? WHERE "messages"."id" = ?
   gate: SAME writer rows=0
10 writer rows=0  UPDATE "rooms" SET "updated_at" = ? WHERE "rooms"."id" = ?
   gate: SAME writer rows=0
11 writer rows=0  COMMIT
   gate: SAME writer rows=0
12 writer rows=1  SELECT "action_text_rich_texts"."id", "action_text_rich_texts"."name", "action_text_rich_tex...
   gate: SAME writer rows=1
13 writer rows=0  insert into message_search_index(rowid, body) values (?, ?)
   gate: SAME writer rows=0
14 writer rows=0  UPDATE "memberships" SET "unread_at" = ?, "updated_at" = ? WHERE "memberships"."room_id" = ?...
   gate: SAME writer rows=0
15 reader rows=1  SELECT "messages"."id", "messages"."room_id", "messages"."creator_id", "messages"."client_me...
   gate: SAME reader rows=1
16 reader rows=1  SELECT "rooms"."id", "rooms"."name", "rooms"."type", "rooms"."creator_id", "rooms"."created_...
   gate: SAME reader rows=1
17 reader rows=1  SELECT "action_text_rich_texts"."id", "action_text_rich_texts"."name", "action_text_rich_tex...
   gate: SAME reader rows=1
18 reader rows=1  SELECT "rooms"."id", "rooms"."name", "rooms"."type", "rooms"."creator_id", "rooms"."created_...
   gate: SAME reader rows=1
19 reader rows=1  SELECT "users"."id", "users"."name", "users"."email_address", "users"."password_digest", "us...
   gate: SAME reader rows=1
20 reader rows=1  SELECT "users"."id", "users"."name", "users"."email_address", "users"."password_digest", "us...
   gate: SAME reader rows=1
21 reader rows=1  SELECT "action_text_rich_texts"."id", "action_text_rich_texts"."name", "action_text_rich_tex...
   gate: SAME reader rows=1
22 reader rows=1  SELECT "action_text_rich_texts"."id", "action_text_rich_texts"."name", "action_text_rich_tex...
   gate: SAME reader rows=1
23 reader rows=0  SELECT b.id, b.key, b.filename, b.content_type, b.metadata, b.service_name, b.byte_size, b.c...
   gate: SAME reader rows=0
24 reader rows=3  SELECT "push_subscriptions".* FROM "push_subscriptions" INNER JOIN "users" ON "users"."id" =...
   gate: SAME reader rows=3
25 reader rows=1  SELECT "action_text_rich_texts"."id", "action_text_rich_texts"."name", "action_text_rich_tex...
   gate: SAME reader rows=1
26 reader rows=1  SELECT COUNT(*) FROM "memberships" WHERE "memberships"."user_id" = ? AND "memberships"."unre...
   gate: SAME reader rows=1
27 reader rows=1  SELECT COUNT(*) FROM "memberships" WHERE "memberships"."user_id" = ? AND "memberships"."unre...
   gate: SAME reader rows=1
28 reader rows=0  SELECT "boosts"."id", "boosts"."message_id", "boosts"."booster_id", "boosts"."content", "boo...
   gate: SAME reader rows=0
29 reader rows=1  SELECT COUNT(*) FROM "memberships" WHERE "memberships"."user_id" = ? AND "memberships"."unre...
   gate: SAME reader rows=1
30 reader rows=1  SELECT "accounts"."id", "accounts"."name", "accounts"."join_code", "accounts"."custom_styles...
   gate: SAME reader rows=1
31 reader rows=6  SELECT "memberships"."id", "memberships"."room_id", "memberships"."user_id", "memberships"."...
   gate: SAME reader rows=6
32 reader rows=1  SELECT "action_text_rich_texts"."id", "action_text_rich_texts"."name", "action_text_rich_tex...
   gate: SAME reader rows=1
33 reader rows=1  SELECT "accounts"."id", "accounts"."name", "accounts"."join_code", "accounts"."custom_styles...
   gate: SAME reader rows=1
gate-only statements (group commit): RELEASE gate_write, SAVEPOINT gate_write
RESULT post: gate list EQUALS the Rust list minus the session and user reads (31 gate statements, 31 expected)
```

The nested FTS5 statements (8 lines starting with `--`) appear inside the FTS insert in both traces.
