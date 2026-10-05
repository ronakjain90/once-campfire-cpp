# Brief for agents that work on a task

Read this before you start. Then read `plans/architecture.md` and your task in `plans/tasks.md`.

## Paths

| Name | Path |
|---|---|
| Workspace | `/Volumes/ExternalHD/Code/AI/once-campfire` |
| Your worktree | `$WORKSPACE/wt/<task ID>` (branch `task/<task ID>`) |
| Rust port (behavior spec, read only) | `$WORKSPACE/once-campfire-rust` at `64f8635` |
| Rails app (ground truth, read only) | `$WORKSPACE/once-campfire-rust/reference` |
| Seeds | `$WORKSPACE/once-campfire-rust/parity/.seed/<name>` |
| Phase 0 gate (example code, capture of Rust traffic) | `$WORKSPACE/once-campfire-cpp/gate/` |

## Environment

- macOS host (Apple M4). Docker runs in a Colima VM (arm64, 8 vCPUs, 16 GB). Do not run
  `colima stop`, `colima start` or `colima delete`.
- The VM mounts `$WORKSPACE` at the same path. Docker bind mounts of paths under it work.
- Build and test in the `campfire-cpp-dev` image (clang 19, CMake, Ninja, OpenSSL, libdeflate,
  jemalloc, zstd, nghttp2, PCRE2, libvips, ffmpeg). Example:
  `docker run --rm -v $WORKSPACE:$WORKSPACE -v cfcpp-build-<task ID>:/build -w $WORKSPACE/wt/<task ID> campfire-cpp-dev bash -lc '...'`
  Put build output in the `/build` volume, not on the workspace mount.
- Images: `campfire-rust:app` (Rust port), `campfire-rust:trace` (Rust port with
  `CAMPFIRE_SQL_TRACE=1` SQL trace), `campfire-reference:app` (Rails), `campfire-bench-runner`
  (has `loadgen`).
- Put database copies for test runs in `/var/lib/campfire-bench/<task ID>/` in the VM, not on the
  workspace mount.
- Host bash is 3.2. Use `/opt/homebrew/bin/bash` for host scripts that need bash 4 or later.
- Prefix the names of your containers with your task ID in lower case. Remove them when you finish.
  Do not stop containers that you did not start.

## Rules

1. Match behavior, not code. Read the Rails source to learn what must happen, and the Rust source
   to learn the exact bytes and the deliberate differences. Then write the C++ the way
   `plans/architecture.md` says. Do not translate Rust line by line.
2. Follow the code rules in `plans/architecture.md`, section 14.
3. Work only in your worktree. Commit on your branch, in small commits with clear messages. End
   each commit message with `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`. Do not
   merge, rebase or push.
4. Do not edit a directory that belongs to another task. If you need a change there, describe it
   in your report.
5. If a requirement is not possible, or the design does not cover a case, stop that part and
   report it. Do not change a requirement by yourself.
6. Never report a result that you did not see. Include the raw output of each acceptance command.
7. A watchdog stops an agent that shows no progress for 10 minutes. To prevent this:
   - Write or edit a file in pieces of at most about 250 lines for each tool call.
   - Run each command that can take more than a few minutes (builds, image builds, fuzz runs,
     benchmarks) in the background. Poll its log with short commands.
   - Commit each time a piece compiles and its tests pass, so that a stop loses little work.

## Report

Keep it under 100 lines plus raw outputs:
- what you built (files and purpose);
- each place where your code does not yet match the Rust or Rails behavior;
- each decision you made that the design did not specify;
- the raw output of the acceptance commands;
- the last commit hash on your branch.
