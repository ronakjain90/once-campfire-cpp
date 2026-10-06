# Campfire in C++: tasks

Design: `plans/architecture.md`. Each task here is one branch. Sonnet writes the code. Opus reviews
each branch against its acceptance list, runs the tests, and merges it into `main`.

## How a task works

1. Opus makes a git worktree at `/Volumes/ExternalHD/Code/AI/once-campfire/wt/<ID>` on the branch
   `task/<ID>`, from the current `main`.
2. The agent works only in that worktree. It commits on its branch. It does not merge, rebase
   `main` or push.
3. The agent builds and tests in Docker with the image `campfire-cpp-dev`
   (`docker/dev.Dockerfile`). Put build directories in a Docker volume, not on the workspace mount.
4. The agent reports: what it built, what it did not do, each place where it differs from the
   Rust behavior, and the raw output of its acceptance commands.
5. Opus reviews, runs the acceptance commands again, and merges.

## Conventions that let tasks run in parallel

- Each directory under `src/` has its own `CMakeLists.txt`, which defines one static library
  (`campfire_<dir>`) and its tests. The top-level `CMakeLists.txt` adds each `src/*` directory
  that has a `CMakeLists.txt`. A task does not edit another task's directory.
- Tests use doctest. Each library has a test executable `test_<dir>`. `ctest` runs all of them.
- Vectors: read them from `spec/vectors/` (the Rust repo's `vectors/`, copied by task T1).
- Controllers register their routes in their own file (`src/app/routes/<area>.inc`). The router
  generator joins them.

## Wave 1: foundation (parallel)

| ID | Task | Depends on |
|---|---|---|
| T1 | **Build and core.** Top-level CMake, the three builds (`release`, `asan`, `tsan`), doctest, `bin/dev` (build and test in Docker), `clang-format`, `clang-tidy`, `AGENTS.md`. `src/core/`: arena, `Out` buffer, `SafeHtml`, errors, log, configuration (all variables of the Rust `config.rs`), clock with frozen time (`CAMPFIRE_FROZEN_TIME`), XXH3, Rails time formats, the `Task` coroutine type and its scheduler interface. Copy the Rust `vectors/` to `spec/vectors/`. | — |
| T2 | **Rails compatibility** (`src/compat/`), all of architecture section 8, with all vector tests. | — |
| T3 | merged (verified by Opus) | `task/T3` | 316 of 658 corpus cases in scope, all pass. Expected values equal the Rails-made corpus in the Rust repo. Fuzz 5 min clean. |
| T4 | **Production image.** `docker/Dockerfile` with the libvips and ffmpeg build stages from the Rust `Dockerfile`, a runtime stage with the same layout (`/rails/storage`), user, ports and entrypoint as `campfire-rust:app`. A placeholder app binary for now. | — |

## Wave 2: frameworks (parallel, after wave 1)

Directory ownership in wave 2: T5 `src/net/`, `src/app/` (only `main.cpp` and the route table
format), `docker/Dockerfile` (the real build step). T6 `src/req/`. T7 `src/db/`,
`tools/schema_gen.py`. T8 `src/views/`, `src/routes/`, `tools/ctc.py`. T9 `src/assets/`,
`third_party/`. T11 `src/cable/`. T12 `src/storage/`. T13 `tools/diffsweep/`.

| ID | Task | Depends on |
|---|---|---|
| T5 | merged (verified by Opus) | `task/T5` | /up and 404 byte-equal to Rust. /up c=16: 189k req/s (Rust 66k, measured by hand). Fuzz coverage of the parser is low (33 edges): fix in H1. |
| T6 | merged (verified by Opus) | `task/T6` | 2,755 Rails params vectors (same file as the Rust repo), `csrf` and `passwords` groups pass. 4 fuzz targets clean. Invalid bcrypt digest returns false (Rails answers 500). |
| T7 | merged (verified by Opus) | `task/T7` | Tests pass in release, asan, tsan. Group commit 198k writes/s vs 72k one-per-transaction. Dependency tracking costs 28% of a 40-row read: optimize in wave 5. `db.write(sched, fn)` takes the scheduler. |
| T8 | merged (verified by Opus) | `task/T8` | 1,134 Rails path cases, 81 named routes, 34 Rails helper goldens, 7 layout goldens pass. Follow-up T8b: Erubi trim rules. |
| T9 | merged (verified by Opus) | `task/T9` | 314/314 digested names equal Rails; 321/321 bodies equal the live Rust app; importmap is a byte-equal substring of the Rust page. Thruster headers (vary, x-cache, compression choice) belong to A8. |
| T10 | merged (verified by Opus) | `task/T10` | 658/658 on all 6 corpus fields. Rules apply to the Rails expected value only. Style text compared in canonical form; the diff sweep checks exact bytes. |
| T8b | merged (verified by Opus) | `task/T8b` | Erubi trim rule applied by `ctc.py`. Expected files made by Erubi in the Rails container. |
| T11 | merged (verified by Opus) | `task/T11` | Rust recorded Cable sessions replay byte for byte (golden file equals the Rust one). Hub: 110M deliveries/s in memory. Fuzz coverage needs `-fsanitize=fuzzer-no-link` on the library: apply the same fix to the T5 parser target in H1. |
| T12 | merged (verified by Opus) | `task/T12` | 15/15 media outputs byte-identical with libvips 8.16.1 and ffmpeg 7.1.5. The 4 video-frame vectors were made on x86; on arm64 our frames equal the Rust and Rails images' frames (checked by Opus). Media tests run in `bin/dev` now. |
| T13 | merged (verified by Opus) | `task/T13` | 553 requests in 9 areas. Rust against Rust: 0 differences. Reports missing C++ routes correctly. No TLS/HTTP/2 checks (A8 adds them). |

## Wave 3: the app (parallel, after wave 2)

Each area includes its models, templates, controllers, channels and jobs. Each area is done when
its requests in the diff sweep pass.

| ID | Area | Rails sources (`reference/app/`) |
|---|---|---|
| A0 | App framework: app state, session cache, concerns, responses, fragment and page caches, errors, and the sign-in flow as proof. Spec `plans/specs/A0.md`. Runs before A1 to A9. | `controllers/concerns/*`, `sessions_controller` |
| A1 | Sign-in, sessions, transfers, first run, join, welcome, bans, the authentication concerns, platform and user agent detection | `controllers/sessions*`, `first_runs`, `users#new/create`, `welcome`, `concerns/*` |
| A2 | Rooms: show, index, open, closed and direct rooms, involvements, refreshes, settings, the sidebar, the room page cache | `controllers/rooms*`, `users/sidebars` |
| A3 | Messages: create, edit, delete, pages, boosts, the bot API, attachments, broadcasts, unread state, mentions, user autocomplete | `controllers/messages*`, `autocompletable` |
| A4 | Accounts, users, profiles, avatars, bots and keys, join codes, logo, custom styles, push subscriptions, QR codes | `controllers/accounts*`, `users/*`, `qr_code` |
| A5 | Search | `controllers/searches` |
| A6 | PWA, the service worker, link unfurl, `/up`, error pages, Active Storage endpoints | `pwa`, `unfurl_links`, Active Storage |
| A7 | The 7 channels, `Turbo::StreamsChannel`, the room stream authorization patch | `channels/*` |
| A8 | Front server: TLS, ACME, HTTP/2, the front cache, timeouts | Rust `crates/kit/src/front/` |
| A9 | Jobs and integrations: Web Push delivery, webhooks, unfurl fetch, banned content removal | `jobs/*`, `models/*` integrations |

## Wave 4: parity

| ID | Task |
|---|---|
| P1 | Run the Playwright parity harness against Rails on all 5 seeds. Fix each failure. |
| P2 | The full diff sweep against Rust passes with no differences except the allowed headers. |
| P3 | Rails starts on a database that the C++ app wrote, and the other way around. Cookies work both ways. |

## Wave 5: performance

| ID | Task |
|---|---|
| B1 | Full benchmark against Rust: all routes of `bench/run`, Cable fan-out, upload. 3 interleaved reps. |
| B2 | Profile each route below 1.5×. Tune. Record each change with before and after numbers. |

## Wave 6: hardening

| ID | Task |
|---|---|
| H1 | Run each fuzz target for at least one hour. Fix each crash. |
| H2 | All tests and the diff sweep pass in the `asan` and `tsan` builds. |
| H3 | Security review of all network input, the cookie and crypto code, the sanitizer, the outbound HTTP policies and ACME. |

## Status

| ID | Status | Branch | Notes |
|---|---|---|---|
| T1 | merged (verified by Opus) | `task/T1` | `test_core` and `test_compat` pass in release, asan and tsan. `bin/dev` needs `seccomp=unconfined` for TSan. |
| T2 | merged (verified by Opus) | `task/T2` | All vector groups pass in release and ASan. `passwords` (bcrypt) and `csrf` groups moved to T6. Needs T1's doctest wiring. |
| T3 | merged (verified by Opus) | `task/T3` | 316 of 658 corpus cases in scope, all pass. Expected values equal the Rails-made corpus in the Rust repo. Fuzz 5 min clean. |
| T4 | merged (verified by Opus) | `task/T4` | Media libraries byte-identical to `campfire-rust:app`. App build step waits for T1's preset names. |
| T5 | merged (verified by Opus) | `task/T5` | /up and 404 byte-equal to Rust. /up c=16: 189k req/s (Rust 66k, measured by hand). Fuzz coverage of the parser is low (33 edges): fix in H1. |
| T6 | merged (verified by Opus) | `task/T6` | 2,755 Rails params vectors (same file as the Rust repo), `csrf` and `passwords` groups pass. 4 fuzz targets clean. Invalid bcrypt digest returns false (Rails answers 500). |
| T7 | merged (verified by Opus) | `task/T7` | Tests pass in release, asan, tsan. Group commit 198k writes/s vs 72k one-per-transaction. Dependency tracking costs 28% of a 40-row read: optimize in wave 5. `db.write(sched, fn)` takes the scheduler. |
| T8 | merged (verified by Opus) | `task/T8` | 1,134 Rails path cases, 81 named routes, 34 Rails helper goldens, 7 layout goldens pass. Follow-up T8b: Erubi trim rules. |
| T9 | merged (verified by Opus) | `task/T9` | 314/314 digested names equal Rails; 321/321 bodies equal the live Rust app; importmap is a byte-equal substring of the Rust page. Thruster headers (vary, x-cache, compression choice) belong to A8. |
| T11 | merged (verified by Opus) | `task/T11` | Rust recorded Cable sessions replay byte for byte (golden file equals the Rust one). Hub: 110M deliveries/s in memory. Fuzz coverage needs `-fsanitize=fuzzer-no-link` on the library: apply the same fix to the T5 parser target in H1. |
| T12 | merged (verified by Opus) | `task/T12` | 15/15 media outputs byte-identical with libvips 8.16.1 and ffmpeg 7.1.5. The 4 video-frame vectors were made on x86; on arm64 our frames equal the Rust and Rails images' frames (checked by Opus). Media tests run in `bin/dev` now. |
| T13 | merged (verified by Opus) | `task/T13` | 553 requests in 9 areas. Rust against Rust: 0 differences. Reports missing C++ routes correctly. No TLS/HTTP/2 checks (A8 adds them). |
| T10 | merged (verified by Opus) | `task/T10` | 658/658 on all 6 corpus fields. Rules apply to the Rails expected value only. Style text compared in canonical form; the diff sweep checks exact bytes. |
| T8b | merged (verified by Opus) | `task/T8b` | Erubi trim rule applied by `ctc.py`. Expected files made by Erubi in the Rails container. |

## Parallel limit

Run **at most 3 worker agents at the same time**, all on Sonnet. More agents use the usage limit
faster and stop all work together. Start the next queued task only when a running one finishes.

## Restart rule

A resumed agent sends its whole transcript again on each step. A long transcript uses the usage
limit fast. To restart a paused task, start a **new** agent with a short prompt: the task row, the
agent brief, and "continue from the commits and files in the worktree". Do not resume an agent
whose transcript is long.
| A0 | in progress (Sonnet) | `task/A0` | |
| A8 | in progress (Sonnet) | `task/A8` | |
