#!/usr/bin/env bash
# Makes the image campfire-cpp:tsan. `docker build` cannot run a TSan program (the personality
# system call is blocked), and the build runs one (the asset generator). So: build the TSan binary
# in the dev container (bin/dev has seccomp=unconfined), then put it in a copy of campfire-cpp:app.
# Usage: [TSAN_BASE=image] tools/tsan-image.sh   (the base image is campfire-cpp:app by default; use an image of the same revision)
set -euo pipefail
cd "$(dirname "$0")/.."
TSAN_BASE=${TSAN_BASE:-campfire-cpp:app}
# The context is in the worktree: bin/dev mounts only the worktree into the container.
ctx=$(mktemp -d "$PWD/.tsan-ctx.XXXXXX")
trap 'rm -rf "$ctx"' EXIT
bin/dev build tsan campfire
cp tools/tsan.supp "$ctx/tsan.supp"
bin/dev run bash -lc "cp /build/tsan/campfire $ctx/campfire && cp /usr/lib/llvm-19/bin/llvm-symbolizer $ctx/llvm-symbolizer"
cat > "$ctx/Dockerfile" <<EOF2
FROM ${TSAN_BASE}
COPY --chmod=755 campfire llvm-symbolizer /usr/local/bin/
COPY tsan.supp /usr/local/etc/tsan.supp
ENV TSAN_OPTIONS=suppressions=/usr/local/etc/tsan.supp:halt_on_error=0:second_deadlock_stack=1
EOF2
docker build -t campfire-cpp:tsan "$ctx"
