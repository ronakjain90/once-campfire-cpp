# Phase 0: the go/no-go gate (task specifications)

Parent plan: `plans/cpp-port.md`, section 3.

The gate answers one question: can a C++ server with the section 5 design be at least 1.5 times
faster than the Rust port? The gate server is throwaway code. It serves only two routes. It must do
the same work as the Rust port for each request, so that the comparison is fair.

## Paths

| Name | Path |
|---|---|
| Workspace | `/Volumes/ExternalHD/Code/AI/once-campfire` |
| Rust repo (read only, except task G3 on its own branch) | `$WORKSPACE/once-campfire-rust` |
| C++ repo | `$WORKSPACE/once-campfire-cpp` |
| Seed | `$WORKSPACE/once-campfire-rust/parity/.seed/default` (`labels.json` has the ids) |
| Rust image | `campfire-rust:app` |
| Rails image | `campfire-reference:app` |

## Environment

- Docker runs in a Colima VM (Ubuntu, kernel 6.8, arm64, 8 vCPUs, 16 GB).
- The VM mounts `$WORKSPACE` at the same path. Docker bind mounts resolve paths in the VM.
- **Do not put a benchmark database on the `$WORKSPACE` mount.** It is virtiofs, and it makes
  SQLite writes slow. Copy each seed to `/var/lib/campfire-bench` in the VM first. Use
  `colima ssh -- sudo ...` to make that directory.
- The macOS host has bash 3.2. Use `/opt/homebrew/bin/bash` for scripts that need bash 4 or later.
  GNU coreutils are in `/opt/homebrew/opt/coreutils/libexec/gnubin`.
- Benchmark CPUs in the VM: app on CPUs 0–3, load generator on CPUs 4–7.

## Rules for all tasks

- Write all new files in the C++ repo, under `gate/`. Do not change files in the Rust repo, except
  as task G3 says.
- Do not change the seed. Copy it.
- Report every number with the command that produced it.
- If you cannot meet a requirement, stop and report it. Do not change the requirement.
- Write a `README.md` in your task directory. It gives the commands to reproduce your result.

---

## G2: benchmark harness in the VM (Sonnet)

**Output:** `gate/bench/`

Make a harness that runs the Rust load generator against any app image, with the app and the load
generator pinned to separate CPUs in the VM.

1. Write `gate/bench/Dockerfile` for a runner image (`campfire-bench-runner`). It contains:
   - the Docker CLI, `python3`, `util-linux` (`taskset`), `procps`, `curl`, `sqlite3`;
   - the load generator, built from `once-campfire-rust/bench/loadgen` in a Rust build stage.
2. Write `gate/bench/run`. It starts the runner with `--network host`, `--pid host`, the Docker
   socket, and the workspace mount. Inside the runner, it does these steps for each app and each
   rep:
   1. Copy the seed to a new VM-local directory.
   2. Set every push subscription endpoint and webhook URL to `127.0.0.1:9`, as
      `neuter_deliveries` in `once-campfire-rust/bench/run` does.
   3. Start the app container with `--network host`, `--cpuset-cpus 0-3`, and the environment
      that `once-campfire-rust/bench/run` gives the Rust app (`env_args`, `parity/.env.reference`).
   4. Wait for `/up`.
   5. Sign in with `loadgen login`. Get the CSRF token with `loadgen scrape`.
   6. Run `loadgen http` pinned to CPUs 4–7: a 2-second warm-up at 4 clients, then 8 seconds at
      1, 16 and 64 clients. Use the routes `room_show` and `post_message` by default. Accept
      `--routes` for the other routes of `bench/run`.
   7. Record the CPU time that the app container used during each measured cell. Read it from the
      container cgroup (`cpu.stat`, `usage_usec`).
   8. Stop the app container. Delete its directory.
3. Accept these options: `--apps` (a comma-separated list of `name=image`), `--reps` (default 3),
   `--routes`, `--secs`, `--concs`, `--out`. Alternate the order of the apps between reps.
4. Write `gate/bench/report`. It prints a Markdown table: the median and the range of req/s,
   p50 and p99 latency, and app CPU ms per request, for each route, concurrency and app.

**Acceptance (I will check each item):**

- `gate/bench/run --apps rust=campfire-rust:app --reps 1` completes with no errors and no non-2xx
  or 3xx responses.
- The app container shows `Cpus_allowed_list: 0-3` in `/proc/1/status`. The load generator
  process shows `4-7`.
- The app database path is not on the virtiofs mount (`stat -f` shows the VM disk).
- `room_show` returns about 24 KB per response with gzip.

---

## G3: capture what the Rust app does for each request (Sonnet)

**Output:** `gate/capture/`

The gate server must run the same SQL and send the same bytes as the Rust app. This task records
them.

1. In the Rust repo, make a local branch `gate-sql-trace`. Do not push it. Add an SQL trace that
   is off by default:
   - Add the `trace` feature to `rusqlite` in the root `Cargo.toml`.
   - In `crates/db/src/database.rs`, `open_connection`: if `CAMPFIRE_SQL_TRACE` is set, call
     `Connection::trace_v2` (or `trace`) for each connection. Write one line per executed statement
     to stderr: the connection role (`reader` or `writer`), the thread id, and the expanded SQL.
2. Build the image `campfire-rust:trace` from the Rust `Dockerfile` on that branch.
3. Run it on a VM-local copy of the seed, with the same environment as task G2. Sign in once.
4. Mark each request in the log, for example with a request to `/up?mark=N` before and after it.
   Then capture the items below. Request each GET route 3 times, and use the third request. The
   cache is warm by then.

   | Request | Capture |
   |---|---|
   | `GET /rooms/<rooms.watercooler>` with `Accept-Encoding: gzip` | All SQL, request headers, response status and headers, raw body, decoded body |
   | the same with `Accept-Encoding: identity` | Response headers and body |
   | `POST /rooms/<rooms.hq>/messages` (the body that `loadgen http --post-room` sends) | All SQL on the reader and writer connections, with the role of each; response status, headers and body |
   | 10 more posts | The number of outbound TCP connections to `127.0.0.1:9` for each post (push and webhooks). Use `tcpdump` or `ss` in the VM. |

5. After the 11 posts, dump these tables from the Rust database: `messages`,
   `action_text_rich_texts`, `memberships`, `rooms`, and the FTS table (`message_search_index`, or
   the name in `db/schema.rb`). Keep only rows that the posts changed.
6. Write `gate/capture/summary.md`. List each statement for each request, in order, with the
   number of rows it returned. Say which statements read the session and the user. Give the table
   names and the columns that each post writes. Give the number of push and webhook connections
   for each post.

**Acceptance (I will check each item):**

- Each statement in `summary.md` appears in the raw log.
- The decoded room body has the same SHA-256 hash in all 3 requests.
- The POST responses have the status and content type that `loadgen` expects.

---

## G4: the gate server (Sonnet)

**Output:** `gate/server/`

**Depends on:** G3 (`gate/capture/`)

A C++23 server that serves `GET /rooms/:id`, `POST /rooms/:id/messages` and `GET /up`. It uses the
design in `plans/cpp-port.md`, section 5. It must do the same per-request work as the Rust app.
Section "Fairness rules" below says what "the same" means.

### Build

- `gate/server/Dockerfile`: a Debian trixie build stage with clang, CMake and Ninja, and a slim
  runtime stage. Image name: `campfire-gate:app`.
- Libraries: the SQLite 3.53 amalgamation (vendored, FTS5 on), OpenSSL 3, libdeflate,
  picohttpparser (vendored), jemalloc.
- Build with `-O2`, LTO and `-march=armv8.2-a+crypto`. Use no other tuning flags.
- The image reads the same environment variables as the Rust image: `SECRET_KEY_BASE`,
  `HTTP_PORT`, `TARGET_PORT` and the database path under `/rails/storage/db/`. It must start in the
  task G2 harness with no change to the harness.

### Design

- Use one worker thread for each CPU in the cpuset (`sched_getaffinity`). Each worker has its own
  `epoll` loop and its own `SO_REUSEPORT` listener on `HTTP_PORT`.
- Use HTTP/1.1 keep-alive only. Parse each request with picohttpparser, with all headers.
- Each worker has its own SQLite read connection and prepared statements. Use the same pragmas as
  the Rust app (`crates/db/src/database.rs`, `open_connection`).
- One writer thread owns the write connection. It takes all queued writes, up to 64, into one
  transaction. Each write runs in its own `SAVEPOINT`. After the `COMMIT`, it returns each result
  to the worker that sent it, through a queue and an `eventfd`.
- Use `wal_autocheckpoint=0` on the writer. A checkpointer thread runs
  `PRAGMA wal_checkpoint(PASSIVE)` when the WAL passes 1,000 pages, as the Rust app does.

### Fairness rules

These rules make the gate a fair measurement. I will check each rule.

1. **SQL.** For each request, run each statement that the Rust app runs (from
   `gate/capture/summary.md`), in the same order, with the same values, and read all the rows.
   There is one exception: you can skip the statements that read the session and the user.
   Use a per-worker cache from the raw `session_token` cookie value to the user id instead.
2. **Cookies.** On a cache miss, verify the `session_token` cookie as Rails does. Port the steps
   from `once-campfire-rust/crates/rails_compat` and `crates/kit/src/cookies.rs`. Test it with
   `once-campfire-rust/vectors/campfire_sessions.json`. If the cookie is not valid, return 302 to
   `/session/new`.
3. **Room page body.** Compute the cache key of the page from the rows that the SQL returned in
   this request: the id and `updated_at` of each message, and the other values that the Rust page
   parts depend on. If the key is in the page cache, send the cached body. Rust does the same
   (`crates/kit`, recorded page parts and gzip reuse). At startup, put the captured body in the
   cache under the key that the startup queries give. Do not use any other way to choose the
   body.
4. **Gzip.** Keep one gzip body (libdeflate, level 6) and one identity body for each cache key.
   Choose by `Accept-Encoding`, as the Rust app does.
5. **Headers.** Send the same header names, in the same order, as the captured Rust response.
   Compute `ETag` from the cache key. Send the current `Date`.
6. **POST.** Parse the form body. Run each write statement that the Rust app runs for a post, in
   the writer group commit, with real values: new ids, the current time, the message body as Rust
   stores it, and the FTS row. Then do these steps after the commit:
   - Render the response body from the captured template, with this post's values.
     Escape the body text as HTML.
   - Render the broadcast HTML of the message once, as the Rust app does for each post.
   - For each push subscription that the Rust app sends to for this post (from task G3), do the
     Web Push encryption with OpenSSL (P-256 ECDH, HKDF, AES-128-GCM, as in RFC 8291). Then try
     one non-blocking TCP connection to `127.0.0.1:9`. Do this on a separate pool of 2 threads.
   - For each webhook connection in the capture, try one TCP connection to `127.0.0.1:9`.
7. **No shortcuts.** Do not cache a response across requests other than the way rule 3 says.
   Do not skip a write. Do not reduce `synchronous` or change `journal_mode`.

### Tests

- `gate/server/tests/compare.sh` starts the Rust image and the gate image on two copies of the same
  seed, signs in, and checks these things:
  - The decoded room page body is byte-identical. The header names and their order are the same.
  - After the same 20 posts to both, the changed rows in the tables from task G3 are equal. Ignore
    ids and timestamps.
- `gate/server/tests/sql_trace.sh` runs the gate server with `GATE_SQL_TRACE=1`. It shows the
  statements for one room page request and one post. Put this list next to the G3 list in
  `gate/server/README.md`.
- The cookie vector test passes.

**Acceptance (I will check each item):**

- `compare.sh` passes.
- Each statement in the G3 list, except the session and user reads, is in the gate trace in the
  same order.
- `gate/bench/run --apps rust=campfire-rust:app,gate=campfire-gate:app --reps 1` completes with no
  errors.

---

## G5: measure and decide (Opus)

1. Run `gate/bench/run --apps rust=campfire-rust:app,gate=campfire-gate:app --reps 3`.
2. Write `bench/results/gate/report.md` with the table and the decision. The pass rules are in
   `plans/cpp-port.md`, section 3.
