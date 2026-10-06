#!/usr/bin/env bash
# Runs the Pebble tests of test_net (src/net/tests/acme_test.cpp): TLS-ALPN-01 end to end.
# Start Pebble, then run the test in the netns of Pebble, so that "campfire.test" is 127.0.0.1 for both.
# Use after `bin/dev build release test_net`. Run from the worktree root.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
workspace="$(cd "$root/../.." && pwd)"
name="$(basename "$root" | tr 'A-Z' 'a-z')"
work="$(mktemp -d "$root/.pebble.XXXXXX")"
trap 'docker rm -f "$name-pebble" >/dev/null 2>&1 || true; rm -rf "$work"' EXIT
id="$(docker create ghcr.io/letsencrypt/pebble:latest)"
docker cp "$id:/test/certs/pebble.minica.pem" "$work/minica.pem"
docker rm "$id" >/dev/null
docker run -d --name "$name-pebble" --add-host campfire.test:127.0.0.1 \
  -e PEBBLE_VA_NOSLEEP=1 -e PEBBLE_AUTHZREUSE=0 \
  ghcr.io/letsencrypt/pebble:latest -config test/config/pebble-config.json >/dev/null
sleep 2
docker run --rm --network "container:$name-pebble" --security-opt seccomp=unconfined \
  -v "$workspace:$workspace" -v "cfcpp-build-$(basename "$root"):/build" \
  -e "PEBBLE_MINICA=$work/minica.pem" \
  campfire-cpp-dev /build/release/src/net/test_net -tc="ACME*"
