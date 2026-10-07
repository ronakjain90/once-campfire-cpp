#!/usr/bin/env bash
# Builds every libFuzzer target with ASan and UBSan in the dev image. Output: /build/fuzz.
# Usage: tools/fuzz-build.sh   (run it from the worktree; it uses bin/dev)
set -euo pipefail
cd "$(dirname "$0")/.."
exec bin/dev run bash -lc 'set -e
  cmake --preset asan -B /build/fuzz -DCAMPFIRE_FUZZ=ON >/dev/null
  targets=$(ninja -C /build/fuzz -t targets all | sed -n "s/^\(fuzz_[a-z0-9_]*\):.*/\1/p" | sort -u)
  ninja -C /build/fuzz -k 0 $targets 2>&1 | tail -n 60
  ls /build/fuzz/src/*/fuzz/fuzz_* | grep -v "\.\(o\|d\)$"'
