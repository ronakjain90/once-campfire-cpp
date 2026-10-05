#!/usr/bin/env bash
# Start campfire-rust:trace on a VM-local copy of the seed (port 4490/4491). Run on the macOS host.
set -euo pipefail
W=/Volumes/ExternalHD/Code/AI/once-campfire
SEED=$W/once-campfire-rust/parity/.seed/default
ENV_FILE=$W/once-campfire-rust/parity/.env.reference
DIR=/var/lib/campfire-bench/g3
docker rm -f g3-app >/dev/null 2>&1 || true
colima ssh -- sudo sh -c "rm -rf $DIR && mkdir -p $DIR/db $DIR/storage && cp -a $SEED/db/. $DIR/db/ && cp -a $SEED/storage/. $DIR/storage/ && python3 - $DIR/db/production.sqlite3 <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.execute(\"UPDATE push_subscriptions SET endpoint = 'https://127.0.0.1:9/push/' || id\")
db.execute(\"UPDATE webhooks SET url = 'http://127.0.0.1:9/hook/' || id\")
db.commit()
PY
chown -R 1000:1000 $DIR"
# env_args for SERVER_CPUS=0-3: 4 cpus -> WEB_CONCURRENCY = JOB_CONCURRENCY = 3
ARGS=()
while IFS= read -r l; do ARGS+=(-e "$l"); done < <(grep -Ev '^(#|$|WEB_CONCURRENCY|JOB_CONCURRENCY|RAILS_MAX_THREADS|RAILS_LOG_LEVEL)' "$ENV_FILE")
ARGS+=(-e WEB_CONCURRENCY=3 -e JOB_CONCURRENCY=3 -e RAILS_MAX_THREADS=5 -e RAILS_LOG_LEVEL=warn -e CAMPFIRE_SQL_TRACE=1)
docker run -d --name g3-app --cpuset-cpus 0-3 --user 1000:1000 --network host -e HTTP_PORT=4490 -e TARGET_PORT=4491 \
  "${ARGS[@]}" -v $DIR/db:/rails/storage/db -v $DIR/storage:/rails/storage/files campfire-rust:trace
for _ in $(seq 1 300); do colima ssh -- curl -fsS -o /dev/null http://127.0.0.1:4490/up 2>/dev/null </dev/null && break; sleep 0.1; done
