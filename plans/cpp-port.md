# Campfire in C++: port plan

Status: draft, 2026-10-05
Target repo: `once-campfire-cpp` (new, local)
Specification: `once-campfire-rust` at `64f8635`, pinned as a submodule
Ground truth: `once-campfire` (Rails), pinned by the Rust repo as `reference/`

## 1. Goal

Make a C++ implementation of Campfire that does the two things below:

1. It has feature parity with the Rust port. It uses the same database, storage layout, cookies,
   URLs and environment variables. Existing installs upgrade with no data migration.
2. It is at least 1.5 times faster than the Rust port on the README benchmark, on the same host.

Goal 2 is not certain. Section 3 defines a gate that measures the chance before the main work
starts. Do not start Phase 1 until the gate passes.

## 2. Non-goals

- Do not change the frontend. JavaScript, CSS and vendored assets ship byte for byte.
- Do not support a database other than SQLite.
- Do not add features. A difference from the Rust port is a bug, unless this plan lists it.

## 3. Phase 0: the go/no-go gate

The Rust port uses about 91 µs of CPU for each room page. Its HTTP and runtime overhead is only
10–17 µs for each request. Thus a faster language alone cannot give 1.5 times. The gain must
come from a different design. The gate measures the best possible result before the port starts.

The task specifications are in `plans/phase0-gate.md`. Sonnet writes the code. Opus checks each
task against its acceptance list before the next task starts.

| ID | Task | Model |
|---|---|---|
| G1 | Build the Rails image, the Rust image and the `default` seed in the Colima VM. | Opus |
| G2 | Benchmark harness in the VM, with the app and the load generator on separate CPUs. | Sonnet |
| G3 | Record the SQL, the headers and the bodies of the Rust app for each benchmark request. | Sonnet |
| G4 | Throwaway C++ server for the room page and the message POST, with the same work per request. | Sonnet |
| G5 | Measure G4 against the Rust image: 3 interleaved reps. Decide. | Opus |

A CPU profile of the Rust app is not part of the gate. Phase 7 adds one if it is necessary.

**Continue only if one of these is true:**

- The G4 room page is at least 1.6 times the Rust room page. (The full app adds cost, so the
  gate needs a margin above 1.5.)
- The G4 message POST is at least 1.5 times the Rust message POST, and the room page is at least
  1.3 times.

If the gate fails, stop. Write the measurements in `bench/results/gate/report.md`.

## 4. Specification and test oracles

### 4.1 What to copy

- The Rust code is the primary specification. Its structure is close to C++, and it passes the
  parity suite against Rails.
- The C++ port adopts all the deliberate differences in the "Known differences" section of the
  Rust `README.md`. For example, it uses `Sec-Fetch-Site` in place of CSRF tokens.
- If the Rust code and Rails disagree on a behavior that the Rust README does not list, Rails is
  correct. Record the case in `plans/divergences.md`.

### 4.2 Oracles

Use three oracles, in this order:

1. **Golden vectors.** The Rust repo has JSON vectors that the Rails app generated
   (`vectors/*.json`, `crates/views/tests/golden/`). Each C++ library must pass them.
2. **Byte diff against Rust.** Run the Rust image and the C++ image on the same seed with a frozen
   clock. Send the same requests to both. Compare status, headers and decoded bodies byte for
   byte. This oracle is fast and exact, so use it for every route.
3. **Parity suite against Rails.** The Rust repo's Playwright harness (`parity/`) compares HTML,
   the accessibility tree, Cable frames and screenshots. It runs any image through
   `PARITY_CANDIDATE_APP_IMAGE`. This oracle is the final gate.

## 5. Architecture

Each decision below states the reason. The reasons come from the Rust profiles in
`once-campfire-rust/plans/perf-attribution.md` and `bench/results/profile-20260929/`.

### 5.1 Process and threads

- Use one process. It replaces Puma, Redis, Resque and Thruster, as the Rust port does.
- Use one worker thread for each CPU. Each worker has its own `epoll` loop and its own
  `SO_REUSEPORT` listener. A connection stays on one worker for its full life.
- Do not use `io_uring`. The default Docker seccomp profile blocks it.
- Do not use an async runtime. Rust spends 11–12% of page CPU in tokio scheduling and hand-offs.

### 5.2 Database

- Each worker thread owns one SQLite read connection. It runs reads inline, with no hand-off.
- Each read connection keeps its prepared statements for the life of the thread.
- One writer thread owns the only write connection.
- **The writer uses group commit.** It takes all queued writes, up to a limit, into one
  transaction. Each write runs in its own `SAVEPOINT`. If one write fails, roll back only its
  savepoint. Run the after-commit hooks of all writes after the one `COMMIT`.
  Reason: the Rust writer commits each post alone, and the writer is the limit for the POST route.
- Port the Rust checkpointer thread. It moves WAL checkpoints off the writer thread.
- Use the SQLite 3.53 amalgamation with the same compile options as the `rusqlite` bundled build.

### 5.3 HTTP

- Parse HTTP/1.1 with `picohttpparser` (vendored).
- Build each response in one buffer, or as an `iovec` list of cached parts. Send it with one
  `writev`.
- Port the Rust front server features: TLS (OpenSSL 3), HTTP/2 (`nghttp2`), ACME, the response
  cache, zstd and gzip, and the Thruster environment variables.

### 5.4 Per-request cost

- **Session cache.** Keep a per-worker map from the raw `session_token` cookie value to the
  verified session and user. The writer clears entries when a session or user changes.
  Reason: Rust decrypts and verifies the cookie, then reads the session, on each request.
- Port the Rust "recorded page parts" design and its cached gzip parts as they are. They give most
  of the Rust page speed.
- Use `libdeflate` for gzip of complete bodies. Use `zlib-ng` only for streams.
- Use `jemalloc`. The Rust profile showed 5–10% gain over glibc malloc.

### 5.5 Action Cable

- Each worker owns the WebSockets of its connections.
- A broadcast encodes each frame once. The hub sends one reference-counted frame to each worker
  through a queue and an `eventfd`. The worker writes the frame to its sockets with `writev`.
- Use a 4–8 KiB read buffer for each socket. Do not zero it.

### 5.6 Libraries

| Need | Library | Note |
|---|---|---|
| Crypto, TLS | OpenSSL 3 | PBKDF2, HMAC, AES-256-GCM, SHA, P-256 ECDH, HKDF |
| HTML parser | Gumbo, as vendored by Nokogiri | Use the exact version in the reference `Gemfile.lock`. Rails uses this parser, so parity is direct. |
| Regular expressions | PCRE2 with JIT | Supports the lookaround that `fancy-regex` gives the Rust port |
| JSON parse | yyjson | Write the JSON writer by hand to match `ActiveSupport::JSON` |
| Password hash | `crypt_blowfish` (vendored) | |
| Media | libvips 8.16.1, ffmpeg 7.1.5 | Copy the build stages of the Rust `Dockerfile` |
| HTTP/2 | nghttp2 | |
| Compression | libdeflate, zlib-ng, libzstd | |
| Tests | doctest | |
| Fuzz tests | libFuzzer | |

### 5.7 Templates

The Rust port has 80 Askama templates. Translate each template into one C++ render function in
`src/views/`, at the same relative path. Keep the static text as `constexpr std::string_view`.
Each function writes into the response buffer. The Rust golden tests check the output.

## 6. Repository layout

| Path | Contents | Rust source |
|---|---|---|
| `src/ruby/` | Ruby string behavior (ERB escape, `to_i`, `Float#to_s`, URL encoding) | `crates/ruby` |
| `src/rails/` | Rails signatures, encryption, serialization, JSON, the clock | `crates/rails_compat` |
| `src/kit/` | HTTP server, context, params, cookies, session, CSRF, responses, gzip, front server | `crates/kit` |
| `src/routes/` | Path helpers | `crates/routes` |
| `src/db/` | Connections, writer, models, queries, fixtures loader | `crates/db` |
| `src/richtext/` | Action Text pipeline: sanitizer, attachments, autolink, plain text | `crates/richtext` |
| `src/storage/` | Active Storage blobs, disk service, variants, previews | `crates/storage` |
| `src/cable/` | Action Cable protocol, WebSocket, pub/sub hub | `crates/cable` |
| `src/assets/` | Propshaft digests, importmap, overrides | `crates/assets` |
| `src/views/` | Templates and view helpers | `crates/views` |
| `src/app/` | Controllers, channels, jobs, integrations, `main` | `crates/campfire` |
| `tests/` | Unit tests and golden-vector tests | `crates/*/tests` |
| `fuzz/` | libFuzzer targets | — |
| `parity/`, `vectors/`, `bench/` | Copied from the Rust repo. Image names changed. | same |
| `spec/` | Submodule: `once-campfire-rust` at `64f8635` | — |

## 7. Rules for code

- Use C++23, CMake and Ninja, and clang.
- Do not use `new` or `delete`. Use values, `std::unique_ptr` and arenas.
- Return errors as `std::expected`. Do not throw across a thread or a C callback.
- Do not keep a `std::string_view` after the buffer that it points to changes or goes away.
- Check bounds in every parser. Each parser that reads network input has a fuzz target.
- Each file starts with a one-line comment that names its Rust source file.
- Comment only behavior that is not obvious. If you match Rails, cite the reference file.
- Format with `clang-format`. Keep `clang-tidy` clean.
- CI builds three ways: release, ASan with UBSan, and TSan. Tests must pass in all three.

## 8. Work breakdown

Each task is one branch in its own git worktree. A task is done when its tests pass in all
three builds and Opus reviews and merges it. Sonnet does well-specified port tasks. Opus does the
design, the core concurrency, the security-critical code and all reviews.

"Review" means a security review by Opus before the merge.

### Phase 1: foundation (Opus, 2–3 hours)

| ID | Task | Depends on |
|---|---|---|
| F1 | Repo, CMake, Dockerfile, CI script, the three builds, doctest, `AGENTS.md` | G5 |
| F2 | Buffer and writer types, errors, log, configuration (port `config.rs`), clock | F1 |
| F3 | Event loop, HTTP/1.1 server, router, request context | F2 |
| F4 | Database layer: read connections, writer with group commit, checkpointer, transaction with after-commit queue | F2 |
| F5 | Byte-diff tool: runs both images on one seed and compares responses for a list of requests | F1 |

### Phase 2: libraries (Sonnet, in parallel, 2–4 hours)

| ID | Task | Rust lines | Oracle | Depends on |
|---|---|---|---|---|
| L1 | Ruby string behavior | 893 | `vectors/ruby_core.json` | F2 |
| L2 | Rails signatures, encryption, serialization. **Review.** | 1,934 | `vectors/rails_compat.json` | F2 |
| L3 | Path helpers | 114 | `vectors/campfire_routes.json` | F2 |
| L4 | `ActiveSupport::JSON` writer | part of L2 | `vectors/rails_compat.json` | F2 |
| L5 | Assets: digests, importmap, overrides | 1,311 | Rust asset tests | F2 |
| L6 | Rich text: vendor Gumbo, port the sanitizer, DOM, attachments, autolink, plain text. **Review.** | 3,531 | 658-case corpus | L1, L2 |
| L7 | Storage: blobs, disk service, Marshal digest, variants, previews | 5,728 | `vectors/storage.json`, images in `vectors/storage/` | L2 |
| L8 | User agent detection | 1,303 | `vectors/campfire_user_agents.json` | L1 |
| L9 | QR code (`rqrcode` port) | 709 | Rust QR tests | F2 |
| L10 | Search word ranges | 776 | Rust search tests | L1 |

### Phase 3: framework (Sonnet, in parallel, 2–4 hours)

| ID | Task | Oracle | Depends on |
|---|---|---|---|
| K1 | Params, form decoding, multipart. Fuzz targets. | Rust `kit` tests | F3 |
| K2 | Cookies, session, flash, `Sec-Fetch-Site` forgery protection, session cache. **Review.** | `vectors/campfire_sessions.json` | F3, L2 |
| K3 | Formats, responses, ETags, gzip, recorded page parts | Rust `kit` tests | F3 |
| K4 | Front server: TLS, HTTP/2, response cache, zstd, timeouts, `X-Forwarded-*` | Rust `kit/tests/front.rs` | K3 |
| K5 | ACME (TLS-ALPN-01, HTTP-01), Thruster certificate storage. **Review.** | Pebble test, as in Rust | K4 |
| K6 | WebSocket and Action Cable protocol, pub/sub hub. Fuzz targets. | Rust `cable` tests, reference frames | F3 |

### Phase 4: models and views (Sonnet, in parallel, 2–4 hours)

| ID | Task | Oracle | Depends on |
|---|---|---|---|
| D1 | Accounts, users, sessions, memberships, bans | Rust `db` tests | F4 |
| D2 | Rooms, messages, boosts, rich texts, with all callbacks in Rails order | Rust `db` tests | F4, L6 |
| D3 | Search (FTS), push subscriptions, webhooks, fixtures loader | Rust `db` tests | F4 |
| V1 | Layout and view helpers, fragment cache | Rust golden tests | L1, L3, L5 |
| V2 | Room and message templates | Rust golden tests | V1 |
| V3 | User, account and session templates | Rust golden tests | V1 |
| V4 | All other templates | Rust golden tests | V1 |

### Phase 5: application (Sonnet, in parallel, 3–6 hours)

Each task passes the byte-diff tool (F5) for all routes it owns.

| ID | Task | Depends on |
|---|---|---|
| A1 | Sign-in, sessions, first run, the hourly session refresh | K2, D1, V3 |
| A2 | Rooms and memberships | D2, V2 |
| A3 | Messages, boosts, attachments | D2, L7, V2 |
| A4 | Users, accounts, profiles, avatars | D1, L7, V3 |
| A5 | Search | D3, L10 |
| A6 | Bot API | A3 |
| A7 | Active Storage endpoints: redirects, proxies, direct uploads, range requests | L7 |
| A8 | PWA manifest, QR codes, user agent pages, other routes | L8, L9, V4 |
| C1 | App channels, Turbo streams, the room stream authorization patch | K6, D1, D2 |
| J1 | Job queues | F4 |
| J2 | Web Push: encryption and delivery. **Review.** | J1, L2 |
| J3 | Bot webhooks | J1 |
| J4 | Link unfurl with the private network guard. **Review.** | J1 |

### Phase 6: parity (Opus with Sonnet, 4–8 hours)

| ID | Task |
|---|---|
| P1 | Run the full Playwright parity suite against Rails on all 5 seeds. Fix each failure. |
| P2 | Run the header shape sweep (`reference-tools/http_shape/sweep.py`). |
| P3 | Start Rails on a database that the C++ app wrote. Start the C++ app on a database that Rails wrote. |
| P4 | Sign in on Rails, then use the cookie on the C++ app, and the other way around. |

### Phase 7: performance (Opus, 4–8 hours)

| ID | Task |
|---|---|
| B1 | Run `bench/run` with the Rust image and the C++ image: 3 interleaved reps. |
| B2 | Profile each route that is below 1.5 times. Tune. Record each change with before and after numbers in `bench/results/`. |
| B3 | Write `bench/results/<date>/report.md` with the final table. |

### Phase 8: hardening (Opus with Sonnet, 3–6 hours)

| ID | Task |
|---|---|
| H1 | Run each fuzz target for at least one hour. Fix each crash. |
| H2 | Run all tests and the byte-diff tool with ASan, UBSan and TSan builds. |
| H3 | Do a security review of each "Review" task and of all network input paths. |

## 9. Schedule

The Rust agent reached the lean parity gate 13.5 hours after its first commit
(2026-09-26 14:18 to 2026-09-27 03:44). It reached the full parity matrix after about 19 hours.
The next four days were performance tuning. The C++ port reuses the vectors, seeds, parity
harness and load generator, which the Rust agent had to build first. C++ needs more time to find
memory and concurrency bugs.

| Phase | Time |
|---|---|
| 0. Gate | 2–4 hours |
| 1. Foundation | 2–3 hours |
| 2–5. Port | 8–16 hours, with 4–6 Sonnet agents in parallel |
| 6. Parity | 4–8 hours |
| 7. Performance | 4–8 hours |
| 8. Hardening | 3–6 hours |
| **Total** | **about 1–2 days of agent time to parity, 2–3 days with tuning** |

The schedule assumes these conditions:

- Docker Desktop has at least 8 CPUs and 16 GB of memory.
- The first build of the media libraries (libvips, ffmpeg) is in the Docker cache after Phase 0.
- The gate passes. If it fails, the total is the gate time only.

## 10. Risks

| Risk | Effect | Response |
|---|---|---|
| The gate shows less than 1.5 times | Goal 2 fails | Stop after Phase 0. |
| The Rust port gets faster while we work | The target moves | Benchmark against the pinned `64f8635`. Report the current Rust `main` also. |
| Memory errors in parsers | Security bugs | Fuzz targets, ASan in CI, reviews by Opus. |
| Group commit changes the order of after-commit events | Cable frames arrive in a different order | Keep the queue order in each batch. Test with the parity Cable checks. |
| The session cache keeps a revoked session | A banned user stays signed in | The writer clears the cache in the same step as the commit. Add a named test for each revocation path. |
| Docker Desktop CPU pinning is not exact | Noisy benchmark numbers | Use interleaved reps and report the range. Compare only ratios on the same host. |
| Agents copy the Rust code without the Rails reason | Parity bugs | Each task cites its Rust and Rails sources. Opus reviews against both. |

## 11. Task status

Agents update this table when they start or finish a task.

| ID | Status | Branch | Notes |
|---|---|---|---|
| G1 | done | | Rails image 4 min, Rust image 5.5 min, seed `default` built |
| G2 | done (verified by Opus) | | Harness in `gate/bench/`. First Rust run was noisy: G3 built an image at the same time. |
| G3 | done (verified by Opus) | `gate-sql-trace` (Rust worktree) | Room page: 9 reads. Post: 5 request reads, 9 writer statements, 19 pool reads. No push or webhook sends. |
| G4 | in progress (Sonnet) | | |
| G5 | not started | | |
