# G3 summary: what the Rust app does for each benchmark request

All numbers come from one run of `run-capture.sh` (see README.md). The raw SQL log is `raw/app.log` (`docker logs -t g3-app`). Per-request slices are in `raw/sql/`. The parsed form is `raw/sql-statements.json`.

Rust image: `campfire-rust:trace` (branch `gate-sql-trace`). The app ran on a VM-local seed copy (`/var/lib/campfire-bench/g3`), port 4490, same env as G2.

A statement line is `start <role> <thread> <expanded SQL>` followed by `end ... rows=N <SQL with ?>`. The `rows` column below is N. Reader thread ids change from request to request, because the app picks a reader from a pool. Only the role matters.

## 1. GET /rooms/486777696 (watercooler), third request

- `Accept-Encoding: gzip`: request `raw/room-gz-3.request.txt`, response headers `raw/room-gz-3.response-headers.txt`, raw body `raw/room-gz-3.body.raw`, decoded body `raw/room-gz-3.body.decoded`.
- `Accept-Encoding: identity`: request `raw/room-id-3.request.txt`, response headers `raw/room-id-3.response-headers.txt`, raw body `raw/room-id-3.body.raw`, decoded body `raw/room-id-3.body.decoded`.

Decoded body SHA-256 (`sha256sum raw/room-*-*.body.decoded`):

| request | status | wire bytes | decoded bytes | SHA-256 of the decoded body |
|---|--:|--:|--:|---|
| gzip #1 | 200 | 24231 | 416122 | `65f5de290894e00109d0b10bb02d63d56e89ead89d4258d7d8e0390e446dc072` |
| gzip #2 | 200 | 24231 | 416122 | `65f5de290894e00109d0b10bb02d63d56e89ead89d4258d7d8e0390e446dc072` |
| gzip #3 | 200 | 24231 | 416122 | `65f5de290894e00109d0b10bb02d63d56e89ead89d4258d7d8e0390e446dc072` |
| identity #1 | 200 | 416122 | 416122 | `65f5de290894e00109d0b10bb02d63d56e89ead89d4258d7d8e0390e446dc072` |
| identity #2 | 200 | 416122 | 416122 | `65f5de290894e00109d0b10bb02d63d56e89ead89d4258d7d8e0390e446dc072` |
| identity #3 | 200 | 416122 | 416122 | `65f5de290894e00109d0b10bb02d63d56e89ead89d4258d7d8e0390e446dc072` |

All 6 hashes are equal. The gzip response has `content-encoding: gzip`, `transfer-encoding: chunked`, `vary: Accept-Encoding`, `etag: W/"7b5fe0b3da7cd2db376df9a202a71ee7"`, `x-cache: miss`. The identity response has `content-length: 416122`. The `x-request-id`, `x-runtime`, `date` and `set-cookie: last_room` expires date change for each request.

**SQL: 9 statements, 9 reader, 0 writer.** All run on one reader thread, in this order. The identity request runs the same 9 statements (checked: same text, same row counts: yes).

Statements 1 and 2 read the session and the user. The 40 messages of the page come from one `messages` SELECT (statement 4). No statement loads their bodies, users or boosts. This is observed, not proven: the Rust app has an in-process fragment cache (`campfire_views::fragment_cache`) that the earlier GETs of the room filled. The first GET of a cold app runs more statements. The response header `x-cache: miss` comes from the proxy and is not this cache.

| # | role | thread | rows | tables | note | statement (expanded, as in raw/app.log) |
|--:|---|--:|--:|---|---|---|
| 1 | reader | 5 | 1 | sessions | SESSION | `SELECT "sessions"."id", "sessions"."user_id", "sessions"."token", "sessions"."ip_address", "sessions"."user_agent", "sessions"."last_active_at", "sessions"."created_at", "sessions"."updated_at" FROM "sessions" WHERE "sessions"."token" = 'vZuXbHrmde1pgqLcyxsVoq6x' LIMIT 1` |
| 2 | reader | 5 | 1 | users | USER | `SELECT "users"."id", "users"."name", "users"."email_address", "users"."password_digest", "users"."role", "users"."status", "users"."bio", "users"."bot_token", "users"."created_at", "users"."updated_at" FROM "users" WHERE "users"."id" = 127326141 LIMIT 1` |
| 3 | reader | 5 | 1 | memberships, rooms |  | `SELECT "rooms"."id", "rooms"."name", "rooms"."type", "rooms"."creator_id", "rooms"."created_at", "rooms"."updated_at" FROM "rooms" INNER JOIN "memberships" ON "rooms"."id" = "memberships"."room_id" WHERE "memberships"."user_id" = 127326141 AND "rooms"."id" = 486777696 LIMIT 1` |
| 4 | reader | 5 | 40 | messages |  | `SELECT "messages"."id", "messages"."room_id", "messages"."creator_id", "messages"."client_message_id", "messages"."created_at", "messages"."updated_at" FROM "messages" WHERE "messages"."room_id" = 486777696 ORDER BY "messages"."created_at" DESC LIMIT 40` |
| 5 | reader | 5 | 1 | rooms |  | `SELECT "rooms"."id", "rooms"."name", "rooms"."type", "rooms"."creator_id", "rooms"."created_at", "rooms"."updated_at" FROM "rooms" ORDER BY "rooms"."created_at" ASC LIMIT 1` |
| 6 | reader | 5 | 1 | accounts |  | `SELECT "accounts"."id", "accounts"."name", "accounts"."join_code", "accounts"."custom_styles", "accounts"."settings", "accounts"."singleton_guard", "accounts"."created_at", "accounts"."updated_at" FROM "accounts" ORDER BY "accounts"."id" ASC LIMIT 1` |
| 7 | reader | 5 | 1 | accounts |  | `SELECT "accounts"."id", "accounts"."name", "accounts"."join_code", "accounts"."custom_styles", "accounts"."settings", "accounts"."singleton_guard", "accounts"."created_at", "accounts"."updated_at" FROM "accounts" ORDER BY "accounts"."id" ASC LIMIT 1` |
| 8 | reader | 5 | 0 | active_storage_attachments, active_storage_blobs |  | `SELECT b.id, b.key, b.filename, b.content_type, b.metadata, b.service_name, b.byte_size, b.checksum, b.created_at FROM active_storage_blobs b JOIN active_storage_attachments a ON a.blob_id = b.id WHERE a.record_type = 'Account' AND a.record_id = 873240054 AND a.name = 'logo' ORDER BY a.id LIMIT 1` |
| 9 | reader | 5 | 1 | memberships, rooms |  | `SELECT "rooms"."id", "rooms"."name", "rooms"."type", "rooms"."creator_id", "rooms"."created_at", "rooms"."updated_at" FROM "rooms" INNER JOIN "memberships" ON "rooms"."id" = "memberships"."room_id" WHERE "memberships"."user_id" = 127326141 AND "rooms"."id" = 486777696 LIMIT 1` |

## 2. POST /rooms/201306877/messages (hq), third post

Request: `raw/post-3.request.txt`. Response: `raw/post-3.response-headers.txt`, `raw/post-3.body.raw`, `raw/post-3.body.decoded`. The request has these headers, in this order: `host`, `cookie`, `content-type: application/x-www-form-urlencoded`, `accept: text/vnd.turbo-stream.html, text/html, application/xhtml+xml`, `x-csrf-token`, `sec-fetch-site: same-origin`, `accept-encoding: gzip`, `content-length`. The body is `message%5Bbody%5D=bench%20write%20N&message%5Bclient_message_id%5D=<nonce>&authenticity_token=<csrf>`.

`loadgen scrape` returns `csrf: null` for this app, so loadgen (and this capture) send an empty `x-csrf-token` and an empty `authenticity_token`. The app accepts that (status 200). `raw/loadgen-wire.txt` is a tcpdump of a real `loadgen http --post-room` request. Its headers and body have the same bytes as `raw/post-3.request.txt`, except the nonce and the cookie. The first line of the body differs only in the number N.

| post | status | content-type | what loadgen expects |
|--:|--:|---|---|
| 1 | 200 | `text/vnd.turbo-stream.html; charset=utf-8` | status < 400 |
| 2 | 200 | `text/vnd.turbo-stream.html; charset=utf-8` | status < 400 |
| 3 | 200 | `text/vnd.turbo-stream.html; charset=utf-8` | status < 400 |
| 4 | 200 | `text/vnd.turbo-stream.html; charset=utf-8` | status < 400 |
| 5 | 200 | `text/vnd.turbo-stream.html; charset=utf-8` | status < 400 |
| 6 | 200 | `text/vnd.turbo-stream.html; charset=utf-8` | status < 400 |
| 7 | 200 | `text/vnd.turbo-stream.html; charset=utf-8` | status < 400 |
| 8 | 200 | `text/vnd.turbo-stream.html; charset=utf-8` | status < 400 |
| 9 | 200 | `text/vnd.turbo-stream.html; charset=utf-8` | status < 400 |
| 10 | 200 | `text/vnd.turbo-stream.html; charset=utf-8` | status < 400 |
| 11 | 200 | `text/vnd.turbo-stream.html; charset=utf-8` | status < 400 |

loadgen counts a response as ok when the status is below 400 (`ok` in `loadgen http`). A real `loadgen http --post-room ... --requests 1` run gave `ok=1`, `errors=0` (`raw/loadgen-validate.json`). Response headers of post 3: `x-cache: bypass`, `content-encoding: gzip`, `transfer-encoding: chunked`, `vary: Accept-Encoding` and `vary: Accept,Accept-Encoding` (two headers), `etag`. The decoded body is a `<turbo-stream action="append" target="messages_rooms_open_201306877">` of about 1.9 KB.

**SQL, post 3: 33 top-level statements = 24 reader + 9 writer. Plus 8 nested FTS5 internal statements on the writer (lines that start with `-- `), which SQLite runs inside the `insert into message_search_index` statement.** The count is the same for all 11 posts (checked by `gen-summary.py`: the multiset of normalized top-level statements is equal). Post 4 has more nested FTS statements (an FTS5 segment merge: 18 nested instead of 8).

- Reader, request thread (statements 1-5, same thread): `bans` by IP, **`sessions` by token (session)**, **`users` by id (user)**, `memberships` by room and user, `rooms` by id. The statement order is: bans, session, user, membership, room.
- Writer: `BEGIN IMMEDIATE`, 4 writes (messages INSERT, action_text_rich_texts INSERT, messages UPDATE, rooms UPDATE), `COMMIT`. After the `COMMIT`, outside the transaction: a `SELECT` of the rich text, the FTS5 insert, and the `memberships` UPDATE.
- Reader, other pool threads (19 statements): the work after the commit. It loads the message, room, user, rich text, attachment, boosts and account for the broadcast and the response. It runs one `push_subscriptions` SELECT (3 rows), and 3 unread counts (`SELECT COUNT(*) FROM memberships ... unread_at IS NOT NULL`), one for each user with a push subscription. These statements read `users` by id (the post creator) but not the session.
- Webhooks: no statement reads the `webhooks` table in the room post, because the message mentions no bot. `deliver_webhooks_to_bots` finds no bot.
- 3 statements start after the client got the response (marked `after-response`). The others start before it.

| # | role | thread | rows | tables | note | statement (expanded, as in raw/app.log) |
|--:|---|--:|--:|---|---|---|
| 1 | reader | 5 | 0 | bans |  | `SELECT 1 AS one FROM "bans" WHERE "bans"."ip_address" = '127.0.0.1' LIMIT 1` |
| 2 | reader | 5 | 1 | sessions | SESSION | `SELECT "sessions"."id", "sessions"."user_id", "sessions"."token", "sessions"."ip_address", "sessions"."user_agent", "sessions"."last_active_at", "sessions"."created_at", "sessions"."updated_at" FROM "sessions" WHERE "sessions"."token" = 'vZuXbHrmde1pgqLcyxsVoq6x' LIMIT 1` |
| 3 | reader | 5 | 1 | users | USER | `SELECT "users"."id", "users"."name", "users"."email_address", "users"."password_digest", "users"."role", "users"."status", "users"."bio", "users"."bot_token", "users"."created_at", "users"."updated_at" FROM "users" WHERE "users"."id" = 127326141 LIMIT 1` |
| 4 | reader | 5 | 1 | memberships |  | `SELECT "memberships"."id", "memberships"."room_id", "memberships"."user_id", "memberships"."involvement", "memberships"."unread_at", "memberships"."connected_at", "memberships"."connections", "memberships"."created_at", "memberships"."updated_at" FROM "memberships" WHERE "memberships"."room_id" = 201306877 AND "memberships"."user_id" = 127326141 LIMIT 1` |
| 5 | reader | 5 | 1 | rooms |  | `SELECT "rooms"."id", "rooms"."name", "rooms"."type", "rooms"."creator_id", "rooms"."created_at", "rooms"."updated_at" FROM "rooms" WHERE "rooms"."id" = 201306877 LIMIT 1` |
| 6 | writer | 8 | 0 |  |  | `BEGIN IMMEDIATE` |
| 7 | writer | 8 | 1 | messages |  | `INSERT INTO "messages" ("client_message_id", "created_at", "creator_id", "room_id", "updated_at") VALUES ('18dba095eb6bd9fb3', '2026-10-05 12:04:02.876379', 127326141, 201306877, '2026-10-05 12:04:02.876379') RETURNING "id"` |
| 8 | writer | 8 | 1 | action_text_rich_texts |  | `INSERT INTO "action_text_rich_texts" ("body", "created_at", "name", "record_id", "record_type", "updated_at") VALUES ('bench write 3', '2026-10-05 12:04:02.876595', 'body', 933434641, 'Message', '2026-10-05 12:04:02.876595') RETURNING "id"` |
| 9 | writer | 8 | 0 | messages |  | `UPDATE "messages" SET "updated_at" = '2026-10-05 12:04:02.876670' WHERE "messages"."id" = 933434641` |
| 10 | writer | 8 | 0 | rooms |  | `UPDATE "rooms" SET "updated_at" = '2026-10-05 12:04:02.876717' WHERE "rooms"."id" = 201306877` |
| 11 | writer | 8 | 0 |  |  | `COMMIT` |
| 12 | writer | 8 | 1 | action_text_rich_texts |  | `SELECT "action_text_rich_texts"."id", "action_text_rich_texts"."name", "action_text_rich_texts"."body", "action_text_rich_texts"."record_type", "action_text_rich_texts"."record_id", "action_text_rich_texts"."created_at", "action_text_rich_texts"."updated_at" FROM "action_text_rich_texts" WHERE "action_text_rich_texts"."record_id" = 933434641 AND "action_text_rich_texts"."record_type" = 'Message' AND "action_text_rich_texts"."name" = 'body' LIMIT 1` |
| 13 | writer | 8 | 0 |  |  | `insert into message_search_index(rowid, body) values (933434641, 'bench write 3')` |
| 14 | writer | 8 | 1 |  | fts-internal | `-- PRAGMA 'main'.data_version` |
| 15 | writer | 8 | 0 | message_search_index_content | fts-internal | `-- INSERT INTO 'main'.'message_search_index_content' VALUES(?1,?2)` |
| 16 | writer | 8 | 1 | message_search_index_docsize | fts-internal | `-- REPLACE INTO 'main'.'message_search_index_docsize' VALUES(?,?)` |
| 17 | writer | 8 | 0 | message_search_index_data | fts-internal | `-- REPLACE INTO 'main'.'message_search_index_data'(id, block) VALUES(?,?)` |
| 18 | writer | 8 | 1 |  | fts-internal | `-- PRAGMA 'main'.data_version` |
| 19 | writer | 8 | 1 | message_search_index_data | fts-internal | `-- REPLACE INTO 'main'.'message_search_index_data'(id, block) VALUES(?,?)` |
| 20 | writer | 8 | 0 | message_search_index_idx | fts-internal | `-- INSERT INTO 'main'.'message_search_index_idx'(segid,term,pgno) VALUES(?,?,?)` |
| 21 | writer | 8 | 0 | message_search_index_data | fts-internal | `-- REPLACE INTO 'main'.'message_search_index_data'(id, block) VALUES(?,?)` |
| 22 | writer | 8 | 0 | memberships |  | `UPDATE "memberships" SET "unread_at" = '2026-10-05 12:04:02.876379', "updated_at" = '2026-10-05 12:04:02.877586' WHERE "memberships"."room_id" = 201306877 AND "memberships"."involvement" != 'invisible' AND ("memberships"."connected_at" IS NULL OR "memberships"."connected_at" < '2026-10-05 12:03:02.877586') AND "memberships"."user_id" != 127326141` |
| 23 | reader | 9 | 1 | messages |  | `SELECT "messages"."id", "messages"."room_id", "messages"."creator_id", "messages"."client_message_id", "messages"."created_at", "messages"."updated_at" FROM "messages" WHERE "messages"."id" = 933434641 LIMIT 1` |
| 24 | reader | 3 | 1 | rooms |  | `SELECT "rooms"."id", "rooms"."name", "rooms"."type", "rooms"."creator_id", "rooms"."created_at", "rooms"."updated_at" FROM "rooms" WHERE "rooms"."id" = 201306877 LIMIT 1` |
| 25 | reader | 3 | 1 | action_text_rich_texts |  | `SELECT "action_text_rich_texts"."id", "action_text_rich_texts"."name", "action_text_rich_texts"."body", "action_text_rich_texts"."record_type", "action_text_rich_texts"."record_id", "action_text_rich_texts"."created_at", "action_text_rich_texts"."updated_at" FROM "action_text_rich_texts" WHERE "action_text_rich_texts"."record_id" = 933434641 AND "action_text_rich_texts"."record_type" = 'Message' AND "action_text_rich_texts"."name" = 'body' LIMIT 1` |
| 26 | reader | 9 | 1 | rooms |  | `SELECT "rooms"."id", "rooms"."name", "rooms"."type", "rooms"."creator_id", "rooms"."created_at", "rooms"."updated_at" FROM "rooms" WHERE "rooms"."id" = 201306877 LIMIT 1` |
| 27 | reader | 9 | 1 | users | USER | `SELECT "users"."id", "users"."name", "users"."email_address", "users"."password_digest", "users"."role", "users"."status", "users"."bio", "users"."bot_token", "users"."created_at", "users"."updated_at" FROM "users" WHERE "users"."id" = 127326141 LIMIT 1` |
| 28 | reader | 3 | 1 | users | USER | `SELECT "users"."id", "users"."name", "users"."email_address", "users"."password_digest", "users"."role", "users"."status", "users"."bio", "users"."bot_token", "users"."created_at", "users"."updated_at" FROM "users" WHERE "users"."id" = 127326141 LIMIT 1` |
| 29 | reader | 9 | 1 | action_text_rich_texts |  | `SELECT "action_text_rich_texts"."id", "action_text_rich_texts"."name", "action_text_rich_texts"."body", "action_text_rich_texts"."record_type", "action_text_rich_texts"."record_id", "action_text_rich_texts"."created_at", "action_text_rich_texts"."updated_at" FROM "action_text_rich_texts" WHERE "action_text_rich_texts"."record_id" = 933434641 AND "action_text_rich_texts"."record_type" = 'Message' AND "action_text_rich_texts"."name" = 'body' LIMIT 1` |
| 30 | reader | 3 | 1 | action_text_rich_texts |  | `SELECT "action_text_rich_texts"."id", "action_text_rich_texts"."name", "action_text_rich_texts"."body", "action_text_rich_texts"."record_type", "action_text_rich_texts"."record_id", "action_text_rich_texts"."created_at", "action_text_rich_texts"."updated_at" FROM "action_text_rich_texts" WHERE "action_text_rich_texts"."record_id" = 933434641 AND "action_text_rich_texts"."record_type" = 'Message' AND "action_text_rich_texts"."name" = 'body' LIMIT 1` |
| 31 | reader | 3 | 0 | active_storage_attachments, active_storage_blobs |  | `SELECT b.id, b.key, b.filename, b.content_type, b.metadata, b.service_name, b.byte_size, b.checksum, b.created_at FROM active_storage_blobs b JOIN active_storage_attachments a ON a.blob_id = b.id WHERE a.record_type = 'Message' AND a.record_id = 933434641 AND a.name = 'attachment' ORDER BY a.id LIMIT 1` |
| 32 | reader | 9 | 3 | memberships, push_subscriptions, users |  | `SELECT "push_subscriptions".* FROM "push_subscriptions" INNER JOIN "users" ON "users"."id" = "push_subscriptions"."user_id" INNER JOIN "memberships" ON "memberships"."user_id" = "users"."id" WHERE ("memberships"."connected_at" IS NULL OR "memberships"."connected_at" < '2026-10-05 12:03:02.878127') AND "memberships"."room_id" = 201306877 AND "memberships"."user_id" != 127326141 AND "memberships"."involvement" = 'everything'` |
| 33 | reader | 9 | 1 | action_text_rich_texts |  | `SELECT "action_text_rich_texts"."id", "action_text_rich_texts"."name", "action_text_rich_texts"."body", "action_text_rich_texts"."record_type", "action_text_rich_texts"."record_id", "action_text_rich_texts"."created_at", "action_text_rich_texts"."updated_at" FROM "action_text_rich_texts" WHERE "action_text_rich_texts"."record_id" = 933434641 AND "action_text_rich_texts"."record_type" = 'Message' AND "action_text_rich_texts"."name" = 'body' LIMIT 1` |
| 34 | reader | 9 | 1 | memberships |  | `SELECT COUNT(*) FROM "memberships" WHERE "memberships"."user_id" = 773523953 AND "memberships"."unread_at" IS NOT NULL` |
| 35 | reader | 9 | 1 | memberships |  | `SELECT COUNT(*) FROM "memberships" WHERE "memberships"."user_id" = 712064548 AND "memberships"."unread_at" IS NOT NULL` |
| 36 | reader | 3 | 0 | boosts |  | `SELECT "boosts"."id", "boosts"."message_id", "boosts"."booster_id", "boosts"."content", "boosts"."created_at", "boosts"."updated_at" FROM "boosts" WHERE "boosts"."message_id" = 933434641 ORDER BY "boosts"."created_at" ASC` |
| 37 | reader | 9 | 1 | memberships |  | `SELECT COUNT(*) FROM "memberships" WHERE "memberships"."user_id" = 149087659 AND "memberships"."unread_at" IS NOT NULL` |
| 38 | reader | 3 | 1 | accounts |  | `SELECT "accounts"."id", "accounts"."name", "accounts"."join_code", "accounts"."custom_styles", "accounts"."settings", "accounts"."singleton_guard", "accounts"."created_at", "accounts"."updated_at" FROM "accounts" ORDER BY "accounts"."id" ASC LIMIT 1` |
| 39 | reader | 3 | 6 | memberships | after-response | `SELECT "memberships"."id", "memberships"."room_id", "memberships"."user_id", "memberships"."involvement", "memberships"."unread_at", "memberships"."connected_at", "memberships"."connections", "memberships"."created_at", "memberships"."updated_at" FROM "memberships" WHERE "memberships"."room_id" = 201306877` |
| 40 | reader | 3 | 1 | action_text_rich_texts | after-response | `SELECT "action_text_rich_texts"."id", "action_text_rich_texts"."name", "action_text_rich_texts"."body", "action_text_rich_texts"."record_type", "action_text_rich_texts"."record_id", "action_text_rich_texts"."created_at", "action_text_rich_texts"."updated_at" FROM "action_text_rich_texts" WHERE "action_text_rich_texts"."record_id" = 933434641 AND "action_text_rich_texts"."record_type" = 'Message' AND "action_text_rich_texts"."name" = 'body' LIMIT 1` |
| 41 | reader | 3 | 1 | accounts | after-response | `SELECT "accounts"."id", "accounts"."name", "accounts"."join_code", "accounts"."custom_styles", "accounts"."settings", "accounts"."singleton_guard", "accounts"."created_at", "accounts"."updated_at" FROM "accounts" ORDER BY "accounts"."id" ASC LIMIT 1` |

## 3. Tables that each post writes

From the writer statements of post 3 (`raw/sql/post-message-3.txt`) and the dump in `tables/`:

| table | operation | columns written |
|---|---|---|
| `messages` | INSERT | `client_message_id`, `created_at`, `creator_id`, `room_id`, `updated_at` (`id` from RETURNING) |
| `action_text_rich_texts` | INSERT | `body` (the canonical HTML, here the text `bench write N`), `created_at`, `name` = 'body', `record_id`, `record_type` = 'Message', `updated_at` |
| `messages` | UPDATE | `updated_at` |
| `rooms` | UPDATE | `updated_at` (room hq) |
| `message_search_index` (FTS5, tokenize=porter) | INSERT | `rowid` = message id, `body` = plain text. Shadow tables `_content`, `_docsize`, `_data`, `_idx` change. |
| `memberships` | UPDATE | `unread_at` (= the message `created_at`), `updated_at`. Rows: room hq, `involvement != 'invisible'`, not connected (or `connected_at` more than 60 s old), `user_id != creator`. It changed 5 rows. |

`push_subscriptions` and `webhooks` are not written. The `memberships` UPDATE has `connected_at` / `connections` unchanged.

### Rows that the posts changed (12 messages: 1 from the loadgen validation run and the 11 captured posts)

- `tables/messages.txt`: 12 rows (NEW or CHANGED; `was` lines show the old values)
- `tables/action_text_rich_texts.txt`: 12 rows (NEW or CHANGED; `was` lines show the old values)
- `tables/memberships.txt`: 5 rows (NEW or CHANGED; `was` lines show the old values)
- `tables/rooms.txt`: 1 rows (NEW or CHANGED; `was` lines show the old values)
- `tables/message_search_index.txt`: 0 rows (NEW or CHANGED; `was` lines show the old values)
- `tables/message_search_index_content.txt`: 12 rows (NEW or CHANGED; `was` lines show the old values)
- `tables/message_search_index-shadow-counts.txt`: row counts of the FTS shadow tables.
- The seed is the base for the comparison: `dump-tables.py` compares the seed DB to a copy of the DB after the posts.

## 4. Outbound connections to 127.0.0.1:9 (push and webhooks)

Method: `tcpdump -i any -n -tt "tcp port 9"` in the Colima VM (`raw/tcpdump-port9.txt`, errors `raw/tcpdump.err`). It ran during the whole capture: all 3+3 GETs, the loadgen validation post, and all 11 posts. A self test (`raw/tcpdump-selftest.txt`: 2 `curl` calls to 127.0.0.1:9) shows that this tcpdump command sees SYN packets to port 9.

| post | window (unix s) | SYN packets to port 9 |
|--:|---|--:|
| 1 | 1791201836.617 - 1791201836.623 | 0 |
| 2 | 1791201839.745 - 1791201839.752 | 0 |
| 3 | 1791201842.874 - 1791201842.882 | 0 |
| 4 | 1791201846.002 - 1791201846.008 | 0 |
| 5 | 1791201849.129 - 1791201849.136 | 0 |
| 6 | 1791201852.255 - 1791201852.260 | 0 |
| 7 | 1791201855.381 - 1791201855.388 | 0 |
| 8 | 1791201858.512 - 1791201858.518 | 0 |
| 9 | 1791201861.638 - 1791201861.642 | 0 |
| 10 | 1791201864.765 - 1791201864.770 | 0 |
| 11 | 1791201867.890 - 1791201867.897 | 0 |

Total SYN packets to port 9 in the whole run: 0. Push connections per post: 0. Webhook connections per post: 0.

Why 0: the seed has push subscriptions (the push SELECT returns 3 rows per post). But G2 sets their endpoints to `https://127.0.0.1:9/push/<id>`. The Rust code (`crates/campfire/src/integrations/web_push.rs`, `crates/db/src/models/push_subscription.rs` `resolved_endpoint_ip`) sends a push only to an `https` endpoint on port 443 with a permitted host. A loopback address on port 9 fails this check, so the app opens no connection. The app still runs the push SELECT and the 3 unread-count SELECTs. A gate server must do the same SELECTs, and must not send to these endpoints. The room post triggers no webhook, because no bot is mentioned.

## 5. How this was checked

- Every statement of the 3 tables above is a line of `raw/app.log`. Check with `grep -F '<statement>' raw/app.log`.
- Decoded body hashes: `sha256sum raw/room-*-?.body.decoded`.
- The trace is off by default (`CAMPFIRE_SQL_TRACE` unset). `raw/app.log` also holds the statements of start-up and of the login. Those are not part of the 3 requests.
