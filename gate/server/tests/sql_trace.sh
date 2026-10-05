#!/opt/homebrew/bin/bash
# Runs the gate with GATE_SQL_TRACE=1, then compares the statements of one warm room page request and
# one post with the G3 list of the Rust app (gate/capture/raw/sql).
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$HERE/lib.sh"
RAW=$HERE/../../capture/raw
mkdir -p "$HERE/.work"; OUT=$(mktemp -d "$HERE/.work/trace.XXXXXX"); fail=0
trap 'docker rm -f g4-trace >/dev/null 2>&1' EXIT
seed_copy $BENCH/g4-trace
start_app g4-trace campfire-gate:app $BENCH/g4-trace 4590 -e GATE_SQL_TRACE=1 -e GATE_WORKERS=1 || exit 1
C=$(login 4590); ROOM=$(label rooms.watercooler); HQ=$(label rooms.hq)
lines() { docker logs g4-trace 2>&1 | wc -l; }
for i in 1 2 3; do
  a=$(lines)
  curl -s -o /dev/null -H "cookie: $C" -H 'accept-encoding: gzip' "http://127.0.0.1:4590/rooms/$ROOM"; sleep 0.3
done
b=$(lines); docker logs g4-trace 2>&1 | sed -n "$((a+1)),${b}p" > "$OUT/room.trace"
for i in 1 2 3; do
  a=$(lines)
  curl -s -o /dev/null -X POST -H "cookie: $C" -H 'content-type: application/x-www-form-urlencoded' -H 'accept: text/vnd.turbo-stream.html, text/html, application/xhtml+xml' \
    -H 'x-csrf-token: ' -H 'sec-fetch-site: same-origin' -H 'accept-encoding: gzip' --data "message%5Bbody%5D=bench%20write%20$i&message%5Bclient_message_id%5D=trace$i&authenticity_token=" "http://127.0.0.1:4590/rooms/$HQ/messages"
  sleep 0.5
done
b=$(lines); docker logs g4-trace 2>&1 | sed -n "$((a+1)),${b}p" > "$OUT/post.trace"
rn python3 "$HERE/sql_trace_compare.py" "$RAW" room "$OUT/room.trace" || fail=1
rn python3 "$HERE/sql_trace_compare.py" "$RAW" post "$OUT/post.trace" || fail=1
echo "--- nested FTS5 statements the gate trace shows inside the FTS insert (start lines):"; grep ' start ' "$OUT/post.trace" | grep -- '-- ' | sed 's/.*ThreadId([0-9]*) //'
rn rm -rf $BENCH/g4-trace
echo "work files: $OUT"; exit $fail
