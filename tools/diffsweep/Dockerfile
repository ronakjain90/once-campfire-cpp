# Runner image for the diff sweep (campfire-diffsweep). It extends campfire-bench-runner (docker
# CLI, python3, curl, sqlite3) with a zstd decoder, Brotli and PyYAML.
FROM campfire-bench-runner
RUN apt-get update \
 && apt-get install -y --no-install-recommends zstd python3-brotli python3-yaml \
 && rm -rf /var/lib/apt/lists/*
