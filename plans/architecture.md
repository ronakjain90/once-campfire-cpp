# Campfire in C++: architecture

Status: approved design, 2026-10-05
Parent plan: `plans/cpp-port.md`. Task list: `plans/tasks.md`.

This document gives the design of the full app. The design starts from what the app must do and
from what C++ does well. It does not copy the structure of the Rust port. The Rust port and the
Rails app are the specification of **behavior** only.

## 1. Requirements

1. **Parity.** The app has all the features of the Rust port, with the same behavior, the same
   bytes on the wire, the same database, the same storage layout and the same cookies. The
   deliberate differences in the "Known differences" section of the Rust `README.md` are part of
   the behavior. Copy them.
2. **Speed.** At least 1.5 times the Rust port on each benchmark route, on the same host.
3. **Safety.** No memory errors. No escape errors. No change in the security behavior of Rails.

## 2. Principles

These rules decide each design question. If a task finds a case that the rules do not cover, ask
Opus.

1. **Do the work once.** Do work at build time, then at boot, then at the time of a cache fill.
   Do work on each request only when no earlier time is possible.
2. **Make wrong code fail to compile.** Use types to keep HTML escape, SQL parameters, units and
   lifetimes correct.
3. **One owner for each mutable thing.** A connection belongs to one worker. The database write
   connection belongs to the writer thread. Shared caches are immutable entries behind atomic
   pointers. Do not share a mutable object between threads without a reason that is written down.
4. **Correct by construction is better than correct by review.** If a mechanism can make a class
   of bug impossible (for example, a cache key that includes all data it uses), use it.
5. **Simple where speed does not matter.** Jobs, sign-in, media and the admin pages are not hot.
   Use blocking code on job threads for them.

## 3. Process and threads

| Thread | Count | Owns |
|---|---|---|
| Worker | One for each CPU in the cpuset | An `epoll` loop, `SO_REUSEPORT` listeners, its connections (HTTP/1.1, HTTP/2, TLS, WebSocket), one SQLite read connection, its prepared statements, its local caches |
| Writer | 1 | The SQLite write connection |
| Checkpointer | 1 | A second write connection, used only for `PRAGMA wal_checkpoint` |
| Job pools | One small pool for each job kind | Blocking work: Web Push, webhooks, link unfurl, media, bcrypt |
| Timer | inside each worker | Timeouts with a hashed timer wheel |

The process listens on `HTTP_PORT` (front: TLS, HTTP/2, plain HTTP) and on `TARGET_PORT` (app,
loopback, trusts `X-Forwarded-*`), as the Rust port does. The same workers serve both.

## 4. Request lifecycle

```
accept → parse (picohttpparser / nghttp2) → route (compile-time table) → Ctx → handler coroutine
      → reads inline → [co_await write] → render → response (headers + body or cached entry) → writev
```

- **Router.** The routes come from `config/routes.rb`. A build-time table matches the method and
  the path segments with no regular expressions and no heap allocation.
- **`Ctx`.** One for each request. It has the parsed request, a per-request arena
  (`std::pmr::monotonic_buffer_resource`), lazy params, lazy cookies, the session, the current
  user (from the session cache), the response builder and the dependency hash (section 6).
- **Handlers are coroutines.** A handler has the type `Task<Response> handler(Ctx&)`. `Task` is a
  small C++20 coroutine type that always resumes on the worker that owns the request.
  - A read runs inline: `auto room = Room::find_for_user(ctx.db(), user_id, room_id);`
  - A write suspends: `auto id = co_await ctx.write([&](Tx& tx) { ... });`
  - Blocking work suspends: `co_await ctx.offload(pool, [&] { ... });`
- **Errors.** Use `std::expected<T, Error>` for results that can fail. A handler that fails gives
  the same status and body as the Rust port (`kit/src/exceptions.rs`). Do not use exceptions for
  control flow. A thrown exception in a handler gives a 500 and a log line, as Rust does for a
  panic.

## 5. Database

- **Schema code generation.** `tools/schema_gen.py` reads `reference/db/schema.rb` and writes
  `src/db/schema.gen.hpp`: one struct for each table, the column list, and a function that reads a
  row into the struct. Text columns in a row are `std::string_view` values in the request arena.
- **Typed statements.** Each SQL statement is a `constexpr` object:
  `constexpr Query<Room(int64_t user_id, int64_t room_id)> RoomForUser{"SELECT ..."};`. Each
  statement has a static index. Each connection keeps an array of prepared statements, indexed by
  that number. The first use prepares the statement. No hash map is on the hot path.
- **The SQL text is the same as the Rust port** when a statement has the same purpose. The G3
  capture (`gate/capture/`) and the trace image `campfire-rust:trace` show the Rust SQL.
- **Pragmas** are the same as the Rust port (`crates/db/src/schema.rs`).
- **Writer.** Group commit, as the gate server does it: up to 64 writes in one transaction, one
  `SAVEPOINT` for each write. If one write fails, roll back only its savepoint.
  - A write is a lambda that gets a `Tx&`. It runs on the writer thread. It does SQL only. It must
    not render, must not do I/O, and must not wait.
  - `tx.after_commit(fn)` queues work. The writer sends the queue back to the worker that sent the
    write. That worker runs it after the commit: broadcasts, pushes, jobs.
  - `tx.changed(table, id)` records a change. After the commit, the writer publishes the changes
    to all workers. The workers use them to clear the session cache and the user cache.
- **Callbacks.** Rails model callbacks run in a fixed order. Put each model's callbacks in its
  model file, and call them in the same order as Rails does. Cite the Rails file.
- **Checkpoints.** The checkpointer thread runs `PASSIVE` checkpoints when the WAL grows, as the
  Rust port does.
- **Search.** FTS5 with the same tokenizer and the same index updates as Rails.

## 6. Caches

| Cache | Key | Value | Where |
|---|---|---|---|
| Session cache | raw `session_token` cookie value | session row, user row | Per worker. Cleared by writer change events. |
| Fragment cache | the Rails key (for example `[message, "presentation-v3"]`) | rendered HTML | Shared, sharded, bounded by bytes (32 MB, `CAMPFIRE_FRAGMENT_CACHE_MB`) |
| Page cache | dependency hash + request facets | identity body, gzip body, ETag | Shared, sharded, bounded by bytes |
| Static assets | logical path | identity, gzip, zstd bodies, built at boot | Immutable, shared |

### 6.1 Page cache keys from the data

The gate server made its page key from a hand-written list of rows. That list can be wrong. The
full app makes the key automatically:

1. A handler that serves a cacheable page opens a tracked scope: `auto deps = ctx.track();`.
2. Each statement that runs in the scope folds the bytes of each returned row into a 128-bit hash
   (XXH3). The statement index and the parameter values go into the hash too.
3. The handler adds the request facets that the page uses: `deps.facet("host", host)`. The facets
   are the host, the browser class from the User-Agent, the format, and the current user's
   `updated_at`.
4. The page cache key is the final hash. On a hit, the handler sends the cached entry. On a miss,
   it renders, compresses once with libdeflate, and stores the entry.
5. If a template reads the clock, or reads a value that is not from tracked SQL and not a facet,
   the response is not cacheable. `ctx.now()` marks the scope as uncacheable.
6. **Audit.** In the debug and sanitizer builds, the app renders the page again on 1 cache hit in
   16 and compares the bytes. A difference is a test failure. This finds missing facets.

The ETag of a cached page is the hex of the key. This is a "Known difference" of the Rust port
too: its ETags hash page parts, not the body.

## 7. Rendering

### 7.1 Template language

Templates are files with the extension `.ct` in `src/views/`, at the same relative path as the
ERB file they replace. `tools/ctc.py` (the Campfire template compiler) turns each file into a C++
function at build time.

| Syntax | Meaning |
|---|---|
| text | Static text, copied as it is |
| `{{ expr }}` | Write `expr` with HTML escape. `expr` is a C++ expression. |
| `{{= expr }}` | Write `expr` with no escape. `expr` must have the type `SafeHtml`. Any other type is a compile error. |
| `{% if c %}` `{% elif c %}` `{% else %}` `{% end %}` | Conditions |
| `{% for x : range %}` `{% end %}` | Loops |
| `{% render path(args) %}` | Call a partial |
| `{% cache key %}` `{% end %}` | Fragment cache, as Rails `cache` |
| `{% params T a, U b %}` | The parameters of the template (first line) |
| `-%}` | Remove the next newline, as ERB `-%>` does |

- Whitespace and newlines must give the same bytes as the ERB output. The ERB file is the
  reference. Put the ERB source path in a comment on the first line.
- Helpers (`link_to`, `image_tag`, `time_tag`, `turbo_frame_tag`, `button_to`, form builders and
  others) are C++ functions in `src/views/helpers/`. They return `SafeHtml` or write to the
  output. Their output must match Rails byte for byte, including attribute order.
- The output is an `Out` object: a chain of arena blocks. Static text is copied with `memcpy`.

### 7.2 Turbo Streams and JSON

- Turbo Stream responses and broadcasts use the same templates.
- JSON responses use a JSON writer that matches `ActiveSupport::JSON` (escape rules, float
  format, key order). It is in `src/compat/`.

## 8. Rails compatibility (`src/compat/`)

Each item has a golden-vector test from the Rust repo's `vectors/`.

- Ruby string behavior: ERB escape, `to_i`, `to_f`, `strip`, `Float#to_s`, `CGI.escape`,
  `ERB::Util.url_encode` (`vectors/ruby_core.json`).
- Key generation (PBKDF2), `MessageVerifier`, `MessageEncryptor` (AES-256-GCM), signed and
  encrypted cookies, signed ids, SGIDs, the Turbo signed stream names, the Marshal subset for
  variant digests (`vectors/rails_compat.json`, `vectors/campfire_sessions.json`,
  `vectors/storage.json`).
- `ActiveSupport::JSON`, `Content-Disposition`, Rack byte ranges.

## 9. Rich text (`src/richtext/`)

- Parse with **Gumbo, at the version that Nokogiri vendors** in the reference `Gemfile.lock`.
  Rails uses this parser, so the parse tree is the same as Rails.
- Port the sanitizer (the Rails and Loofah safe lists), the Action Text attachment handling
  (mentions by SGID, opengraph embeds, the User SGID fallback), autolink, and the plain text
  extraction.
- Serialize the tree as Nokogiri does. The Rust `crates/richtext/src/dom.rs` has the rules.
- Test with the Rust rich text corpus (658 cases, with 400 fuzzed cases).

## 10. Action Cable (`src/cable/`)

- The WebSocket upgrade and framing run in the worker that owns the connection. A connection stays
  on its worker.
- Use `permessage-deflate` with no context takeover. Compress each broadcast once for all
  subscribers. This is a Rust "Known difference".
- **Hub.** For each stream name, the hub keeps the set of workers that have a subscriber. A
  broadcast encodes the frame once (plain and deflated), and sends one reference-counted frame to
  each of those workers through a queue and an `eventfd`. Each worker writes the frame to its
  subscribers with `writev`, and joins several frames in one call when it can.
- Read buffers are 4 KiB. They grow only for large messages.
- The 7 app channels, `Turbo::StreamsChannel` and the room stream authorization patch are in
  `src/app/channels/`.
- Limits are as in the Rust README: 64 subscriptions for each connection, 4 KiB identifiers,
  1 MiB messages, a 30-second write stall limit.

## 11. HTTP, TLS and the front server (`src/net/`)

- HTTP/1.1 with picohttpparser. HTTP/2 with nghttp2. TLS with OpenSSL 3, with ALPN.
- ACME (TLS-ALPN-01 and HTTP-01) with certificate storage that Thruster can read.
- The front response cache, timeouts, body limits and `X-Forwarded-*` rules are the same as the
  Rust port (`crates/kit/src/front/`).
- A response is a header buffer and a body. The body is a slice of a cached entry (held by a
  reference count) or a buffer in the arena. One `writev` sends both.

## 12. Storage and media (`src/storage/`)

- Active Storage: blobs, attachments, the disk service layout `storage/files/ab/cd/<key>`,
  checksums, signed blob and representation URLs, the redirect and proxy endpoints, direct
  uploads, range requests.
- Variants with the libvips C API (8.16.1). Video posters and metadata with the ffmpeg libraries
  (7.1.5). Copy the build stages of the Rust `Dockerfile`, so the bytes of thumbnails are the same.
- Media work runs on a pool of at most 4 threads, off the writer.

## 13. Jobs and outbound HTTP (`src/jobs/`)

- In-process queues, one for each job kind, with `JOB_CONCURRENCY` threads each.
- One small blocking HTTP/1.1 client with TLS (OpenSSL), timeouts and redirect limits.
- Three outbound policies, never one shared policy: Web Push, bot webhooks, link unfurl. The
  unfurl client resolves DNS through the private network guard, pins the IP, and checks each
  redirect.
- Web Push encryption (RFC 8291) and VAPID (RFC 8292) with OpenSSL.

## 14. Code rules

- C++23, clang, CMake, Ninja. Linux only (`epoll`). Build and run in Docker.
- No `new` or `delete`. Values, `std::unique_ptr`, arenas.
- No `std::string_view` that lives longer than its buffer. Arena views live for one request.
- `std::expected` for errors. No exceptions for control flow.
- Each parser of network input has a libFuzzer target.
- Each file starts with a one-line comment that names the Rails file (and the Rust file, if
  useful) that it matches.
- Comment only behavior that is not obvious. If the code copies a Rails or Rust behavior that
  looks wrong, say so and cite the source.
- `clang-format` and `clang-tidy` stay clean.
- Three builds: release (`-O2`, LTO), `asan` (ASan + UBSan), `tsan`. All tests pass in all three.

## 15. Repository layout

| Path | Contents |
|---|---|
| `src/core/` | Arena, buffers, errors, log, config, clock, hashes, time formats, `Task` |
| `src/net/` | Event loop, HTTP/1.1, HTTP/2, TLS, ACME, front cache, WebSocket framing, router |
| `src/compat/` | Ruby and Rails compatibility |
| `src/db/` | Connections, statements, writer, checkpointer, generated schema |
| `src/models/` | Queries and callbacks for each model |
| `src/richtext/` | Gumbo, sanitizer, attachments, autolink, plain text |
| `src/storage/` | Active Storage, libvips, ffmpeg |
| `src/cable/` | Action Cable protocol and hub |
| `src/views/` | `.ct` templates, helpers, generated code |
| `src/assets/` | Asset digests, importmap, precompressed bodies |
| `src/app/` | Controllers, channels, jobs, integrations, `main.cpp` |
| `src/jobs/` | Queues, outbound HTTP client |
| `tools/` | `schema_gen.py`, `ctc.py`, the diff sweep |
| `tests/` | Unit tests, vector tests, diff sweep lists |
| `fuzz/` | libFuzzer targets |
| `vendor/` | SQLite, picohttpparser, Gumbo, crypt_blowfish, yyjson |
| `third_party/rails-assets/` | Vendored frontend assets, copied from the Rust repo |
| `docker/` | Development image, production image |

## 16. Oracles

1. **Vectors.** `vectors/*.json` from the Rust repo (made by the Rails app).
2. **Diff sweep against Rust.** `tools/diffsweep` runs `campfire-rust:app` and `campfire-cpp:app`
   on the same seed with a frozen clock (`CAMPFIRE_FROZEN_TIME`). It sends the same list of
   requests to both, including sign-in, writes and Cable sessions. It compares the status, the
   headers (names, order, values except `date`, `x-request-id`, `x-runtime`) and the decoded
   bodies. A task is done only when its routes pass.
3. **Parity suite against Rails.** The Playwright harness in `parity/`, with
   `PARITY_CANDIDATE_APP_IMAGE=campfire-cpp:app`. This is the final gate.
