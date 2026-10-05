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
| T3 | **Rich text core** (`src/richtext/`): vendor Gumbo at Nokogiri's version, the DOM, the serializer, the sanitizer. Tests from the Rust corpus for the cases without attachments. | — |
| T4 | **Production image.** `docker/Dockerfile` with the libvips and ffmpeg build stages from the Rust `Dockerfile`, a runtime stage with the same layout (`/rails/storage`), user, ports and entrypoint as `campfire-rust:app`. A placeholder app binary for now. | — |

## Wave 2: frameworks (parallel, after wave 1)

| ID | Task | Depends on |
|---|---|---|
| T5 | **Server.** `src/net/`: event loop, HTTP/1.1, the router generator, `Ctx`, the response builder, timers, worker threads, the cross-worker queues. | T1 |
| T6 | **Request parts.** Rack-compatible params (nested keys), multipart, cookies, encrypted session, flash, `Sec-Fetch-Site` forgery protection with the old-token path, formats and `Accept` rules, bcrypt (vendored `crypt_blowfish`). Vector groups `csrf` and `passwords` of `rails_compat.json`. Fuzz targets for each parser. | T1, T2 |
| T7 | **Database.** `src/db/`: connections, typed statements, `schema_gen.py`, the writer with group commit and after-commit hand-back, change events, the checkpointer, the tracked dependency hash. | T1 |
| T8 | **Templates.** `tools/ctc.py`, `src/views/` foundation: tag helpers with Rails attribute order, URL helpers (`vectors/campfire_routes.json`), form builders, `time_tag`, `turbo_frame_tag`, the layout and the shared partials. | T1, T2 |
| T9 | **Assets.** `src/assets/`: Propshaft digests, importmap, the overrides, precompressed bodies, and the vendored frontend files. | T1 |
| T10 | **Rich text attachments**: mentions (SGID), opengraph embeds, autolink, plain text. All corpus tests. | T2, T3 |
| T11 | **Cable core.** WebSocket framing, `permessage-deflate`, the Action Cable protocol, the hub. Unit tests with the Rust reference frames. | T1 |
| T12 | **Storage.** Blobs, the disk service, signed URLs, the Marshal variant digest, libvips variants, ffmpeg analysis and posters (`vectors/storage.json`, `vectors/storage/`). | T1, T2, T4 |
| T13 | **Diff sweep.** `tools/diffsweep`: runs both images on one seed with a frozen clock, sends a list of requests (sign-in, pages, writes, Cable sessions), compares the results. Request lists from `parity/screens.yml` and `reference-tools/http_shape/sweep.py`. | T4 |

## Wave 3: the app (parallel, after wave 2)

Each area includes its models, templates, controllers, channels and jobs. Each area is done when
its requests in the diff sweep pass.

| ID | Area | Rails sources (`reference/app/`) |
|---|---|---|
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
| T3 | in progress (Sonnet) | `task/T3` | |
| T4 | merged (verified by Opus) | `task/T4` | Media libraries byte-identical to `campfire-rust:app`. App build step waits for T1's preset names. |
