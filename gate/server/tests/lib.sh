#!/usr/bin/env bash
# Shared helpers for the G4 tests. Source this file. Needs bash 4 (use /opt/homebrew/bin/bash on the host).
WORKSPACE=/Volumes/ExternalHD/Code/AI/once-campfire
SEED=$WORKSPACE/once-campfire-rust/parity/.seed/default
ENVF=$WORKSPACE/once-campfire-rust/parity/.env.reference
BENCH=/var/lib/campfire-bench
RUNNER=campfire-bench-runner

# lg <args>: run a command in the runner image on the host network (curl, loadgen, python3, sqlite3).
runner() { docker run --rm --network host -v "$WORKSPACE:$WORKSPACE" -v "$BENCH:$BENCH" "$@"; }
rn() { runner "$RUNNER" "$@"; }

# seed_copy <dir>: VM-local copy of the seed with push/webhook endpoints neutered (as the G2 harness does).
seed_copy() {
  rn sh -c "rm -rf $1 && mkdir -p $1 && cp -a $SEED/db $1/db && cp -a $SEED/storage $1/storage && python3 - <<'PY'
import sqlite3
d=sqlite3.connect('$1/db/production.sqlite3')
d.execute(\"UPDATE push_subscriptions SET endpoint = 'https://127.0.0.1:9/push/' || id\")
d.execute(\"UPDATE webhooks SET url = 'http://127.0.0.1:9/hook/' || id\")
d.commit()
PY
chown -R 1000:1000 $1"
}

# start_app <name> <image> <dir> <port> [extra docker args]
start_app() {
  local name=$1 image=$2 dir=$3 port=$4; shift 4
  docker rm -f "$name" >/dev/null 2>&1 || true
  local envargs=()
  while IFS= read -r l; do envargs+=(-e "$l"); done < <(grep -Ev '^(#|$|WEB_CONCURRENCY|JOB_CONCURRENCY|RAILS_MAX_THREADS|RAILS_LOG_LEVEL)' "$ENVF")
  docker run -d --name "$name" --network host --user 1000:1000 --cpuset-cpus 0-3 -e HTTP_PORT=$port -e TARGET_PORT=$((port + 1)) \
    -e WEB_CONCURRENCY=3 -e JOB_CONCURRENCY=3 -e RAILS_MAX_THREADS=5 -e RAILS_LOG_LEVEL=warn "${envargs[@]}" "$@" \
    -v "$dir/db:/rails/storage/db" -v "$dir/storage:/rails/storage/files" "$image" >/dev/null
  local i
  for i in $(seq 1 300); do curl -fsS -o /dev/null "http://127.0.0.1:$port/up" 2>/dev/null && return 0; sleep 0.1; done
  docker logs "$name" 2>&1 | tail -20 >&2
  return 1
}

label() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$SEED/labels.json" "$1"; }
# login <port> -> prints "session_token=..." cookie header
login() {
  rn loadgen login --base "http://127.0.0.1:$1" --email "$(label emails.david)" --password "$(label passwords.all)" |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["cookie"])'
}
