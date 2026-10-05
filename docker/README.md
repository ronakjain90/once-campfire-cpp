# Docker images

| File | Image | Purpose |
|---|---|---|
| `dev.Dockerfile` | `campfire-cpp-dev` | Build and test toolchain. |
| `Dockerfile` | `campfire-cpp:app` | Production image. A drop-in for `campfire-rust:app`. |

## Build the production image

Run this command from the repository root. The first build compiles libvips and ffmpeg and takes
several minutes. Docker caches the result.

    docker build -t campfire-cpp:app -f docker/Dockerfile .

Optional build arguments: `APP_VERSION`, `GIT_REVISION`, `OCI_DESCRIPTION`, `OCI_SOURCE`.

## Stages

1. `media-base`, `vips`, `ffmpeg`, `media`: copied from the Rust `Dockerfile`. The same sources,
   versions, checksums and configure options. Only the base image differs: `debian:trixie` with
   `build-essential`, not `rust:trixie`. The stages install headers, libraries and `.pc` files in
   `/opt/vips` and `/opt/ffmpeg`.
2. `cpp-build`: clang 19, CMake, Ninja and the libraries of `dev.Dockerfile`. It uses the
   libvips and ffmpeg of stage 1 (`PKG_CONFIG_PATH`). It fails if `pkg-config` does not report
   libvips 8.16.1 and libavcodec 61.19.101.
3. Runtime: `debian:trixie-slim`, user `rails` (1000:1000), `/rails`, `/rails/storage/{db,files,backups}`,
   ports 80 and 443, `CMD ["bin/boot"]`.

## Placeholder

- `docker/placeholder/main.cpp` is the app binary for now. It answers `GET /up` with 200 on
  `HTTP_PORT` (default 80). Every other path returns 404.
- When a top-level `CMakeLists.txt` exists, the `cpp-build` stage runs
  `cmake --preset release && cmake --build --preset release` and installs
  `build/release/campfire`. That branch is not tested yet. Check the binary path against the
  real preset.
- `/hooks/pre-backup` calls `campfire backup`. The placeholder does not support it.
- `bin/boot` runs `campfire server`. The placeholder ignores the argument.
- `docker/hooks/post-restore` is a copy of the reference script.
