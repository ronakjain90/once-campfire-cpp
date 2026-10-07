# Campfire C++ port: guide for agents

Design: `docs/architecture.md`. Known differences: `docs/divergences.md`.

## Build and test

Run all commands through `bin/dev`. It runs them in the `campfire-cpp-dev` image. The build
directory is a Docker volume for each worktree (`cfcpp-build-<worktree name>`), mounted at `/build`.

| Command | Action |
|---|---|
| `bin/dev build [preset] [target...]` | Configure and build. Presets: `release` (default), `asan`, `tsan`. |
| `bin/dev test [preset] [ctest args]` | Run `ctest` for a preset. |
| `bin/dev format [--check] [paths]` | Run `clang-format`. `--check` changes no file. |
| `bin/dev tidy [paths]` | Run `clang-tidy` (default path: `src`). It uses the release build. |
| `bin/dev shell` | Open a shell in the container. |
| `bin/dev run <command...>` | Run a command in the container. |
| `bin/dev clean [preset]` | Delete the build directory. |

The script works with bash 3.2 and with `/opt/homebrew/bin/bash`.

Presets (`CMakePresets.json`):

- `release`: `-O2`, thin LTO with lld, jemalloc, `-march=armv8.2-a+crypto` on arm64 and
  `-march=x86-64-v2` on x86-64.
- `asan`: ASan and UBSan, `-O1 -g`, no LTO, no jemalloc.
- `tsan`: TSan, `-O1 -g`, no LTO, no jemalloc.

All presets set `CAMPFIRE_WERROR=ON`: a warning in a Campfire target is an error.
`bin/dev` runs Docker with `seccomp=unconfined` because TSan needs the `personality` system call.

## How to add a library

The top-level `CMakeLists.txt` adds each `src/<dir>/` that has a `CMakeLists.txt`. Do not edit it.
Create `src/<dir>/CMakeLists.txt` and use these functions (`cmake/CampfireHelpers.cmake`):

```cmake
campfire_library(<dir>
  SOURCES a.cpp b.cpp
  PUBLIC_DEPS campfire_core      # linked PUBLIC: users of the library get them too
  DEPS some_private_target       # linked PRIVATE
  [WERROR])                      # -Werror, also without the preset

campfire_test(<dir>
  SOURCES tests/a_test.cpp
  [DEPS extra_targets])

campfire_executable(<name> SOURCES main.cpp [DEPS ...])
```

- `campfire_library(<dir>)` makes the static library `campfire_<dir>` (alias `campfire::<dir>`).
  The include root is `src/`: write `#include "core/out.hpp"`. The library gets the warning
  options of the project.
- `campfire_test(<dir>)` makes the executable `test_<dir>`. It links `campfire_<dir>` (if the
  target exists), doctest and the shared `main` (`tests/doctest_main.cpp`). It registers the
  executable with `ctest`.
- `campfire_executable` links jemalloc in the release build.
- Vendored libraries are INTERFACE targets in `vendor/CMakeLists.txt`: `campfire_xxhash`,
  `campfire_doctest`. Code that you vendor gets its own target in its own `src/<dir>` or
  `vendor/<name>/` and keeps the include path `SYSTEM`.
- Library order: name a dependency in `PUBLIC_DEPS` or `DEPS`. CMake does the rest.
- Test files use doctest: `#include <doctest.h>`. Vectors are in `tests/vectors/`.

## Code rules

- Follow `docs/architecture.md` section 14.
- `clang-format` (`.clang-format`) and `clang-tidy` (`.clang-tidy`) must be clean:
  `bin/dev format --check` and `bin/dev tidy <paths>`.
- Put a one-line comment at the top of each file that names the Rails file (and the Rust file).
- Write comments, commit messages and documents in ASD-STE100 Simplified Technical English.

## `src/core` overview

| Header | Content |
|---|---|
| `arena.hpp` | `Arena`: per-request `pmr` bump allocator |
| `out.hpp` | `Out`: chunked output buffer, `iovec` output, contiguous copy |
| `html.hpp` | `SafeHtml`, `html_escape` (ERB::Util.html_escape) |
| `error.hpp` | `Error`, `Errc`, `Result<T>`, `Status`, `fail()` |
| `log.hpp` | `Logger`, `log_info(...)`, level from `RAILS_LOG_LEVEL` |
| `config.hpp` | `Config` (app variables), `FrontConfig` (Thruster variables) |
| `timestamp.hpp`, `time_format.hpp` | `Timestamp`, Rails time formats |
| `clock.hpp` | `Clock`, `SystemClock`, `TestClock`, `CAMPFIRE_FROZEN_TIME` |
| `xxh3.hpp` | XXH3 128-bit hash |
| `scheduler.hpp`, `task.hpp` | `Scheduler`, `QueueScheduler`, `Task<T>`, `Completion<T>`, `Yield`, `SuspendHook` |

### Task and thread rules

- A `Task` runs on the thread of its `Scheduler`. `co_await` of a `Task` stays on that thread.
- To wait for work on another thread, call `make_completion<T>(scheduler)`. Give the
  `CompletionSetter` to the other thread, and `co_await` the `Completion`. The setter posts the
  coroutine back to the scheduler. The scheduler resumes it on its own thread.
- Do not destroy a `Task` while it waits for a `Completion` that is not done.
- The worker event loop is the real `Scheduler` (epoll). `QueueScheduler` is for tests and tools.
- `Completion` and `Yield` call the `SuspendHook` of the thread before a coroutine waits. The worker
  uses it to end the read transaction of the request (`docs/architecture.md` section 5). A new
  awaiter that gives the thread back to the scheduler must call the hook too.
