#!/bin/bash
# Builds fuzz_richtext_sanitize with clang and libFuzzer, address and undefined sanitizers.
# Usage: build.sh <output path>
set -e
here="$(cd "$(dirname "$0")" && pwd)"
rt="$here/.."
out="${1:-fuzz_richtext_sanitize}"
tmp="$(mktemp -d)"
san="-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined"
for f in "$rt"/vendor/gumbo/*.c; do
  clang -O1 -g -w $san -c "$f" -o "$tmp/$(basename "$f").o"
done
clang++ -std=c++23 -O1 -g -Wall -Wextra $san -I"$rt/.." -I"$rt/vendor/gumbo" \
  "$here/fuzz_richtext_sanitize.cpp" "$rt/dom.cpp" "$rt/sanitizer.cpp" "$rt/filters.cpp" \
  "$tmp"/*.o -fsanitize=fuzzer,address,undefined -o "$out"
