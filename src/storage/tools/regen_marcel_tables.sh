#!/usr/bin/env bash
# Regenerates src/storage/marcel_tables.cpp with the bundle of the reference image (Marcel 1.1.0).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
image="${IMAGE:-campfire-reference:app}"
docker run --rm -v "$here:/tools:ro" "$image" bundle exec ruby /tools/dump_marcel_tables.rb > "$here/../marcel_tables.cpp"
