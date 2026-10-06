#!/bin/bash
# Builds fuzz_richtext_sanitize with clang and libFuzzer, address and undefined sanitizers.
# The target runs the full pipeline: sanitize, attachments, autolink, plain text, editor value.
# Usage: build.sh <output path>
set -e
here="$(cd "$(dirname "$0")" && pwd)"
rt="$here/.."
src="$rt/.."
out="${1:-fuzz_richtext_sanitize}"
tmp="$(mktemp -d)"
san="-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined"
for f in "$rt"/vendor/gumbo/*.c; do
  clang -O1 -g -w $san -c "$f" -o "$tmp/$(basename "$f").o"
done
clang++ -std=c++23 -O1 -g -Wall -Wextra $san -I"$src" -I"$rt/vendor/gumbo" \
  "$here/fuzz_richtext_sanitize.cpp" "$rt"/dom.cpp "$rt"/sanitizer.cpp "$rt"/filters.cpp "$rt"/text_util.cpp \
  "$rt"/tree.cpp "$rt"/uri.cpp "$rt"/attachables.cpp "$rt"/plain_text.cpp "$rt"/content.cpp \
  "$rt"/message_filters.cpp "$rt"/autolink.cpp "$rt"/richtext.cpp "$rt"/resolver.cpp \
  "$src"/compat/*.cpp "$tmp"/*.o -lcrypto -fsanitize=fuzzer,address,undefined -o "$out"
