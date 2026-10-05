#!/opt/homebrew/bin/bash
# Starts the Rust image and the gate image on two copies of the same seed, signs in, and checks:
#  - the decoded room page body is byte-identical, and the response header names and order are the same
#  - after the same 20 posts to both, the rows the posts change are equal (ids and timestamps ignored)
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$HERE/lib.sh"
set -u
RUST_PORT=4690; GATE_PORT=4590
mkdir -p "$HERE/.work"; OUT=$(mktemp -d "$HERE/.work/cmp.XXXXXX"); fail=0
cleanup() { docker rm -f g4-cmp-rust g4-cmp-gate >/dev/null 2>&1; }
trap cleanup EXIT
seed_copy $BENCH/g4-cmp-rust; seed_copy $BENCH/g4-cmp-gate
start_app g4-cmp-rust campfire-rust:app $BENCH/g4-cmp-rust $RUST_PORT || exit 1
start_app g4-cmp-gate campfire-gate:app $BENCH/g4-cmp-gate $GATE_PORT || exit 1
CR=$(login $RUST_PORT); CG=$(login $GATE_PORT)
ROOM=$(label rooms.watercooler); HQ=$(label rooms.hq)
names() { tr -d '\r' < "$1" | sed -n '2,$p' | grep -v '^$' | cut -d: -f1 | tr 'A-Z' 'a-z'; }
check() { if [ "$2" = ok ]; then echo "PASS  $1"; else echo "FAIL  $1"; fail=1; fi; }

for enc in gzip identity; do
  for app in rust gate; do
    if [ $app = rust ]; then port=$RUST_PORT; ck=$CR; else port=$GATE_PORT; ck=$CG; fi
    for i in 1 2 3; do   # the third request is the warm one, as in G3
      curl -s -D "$OUT/$app-$enc.h" -o "$OUT/$app-$enc.raw" -H "cookie: $ck" -H "accept-encoding: $enc" -H "accept:" -H "host: 127.0.0.1:4390" "http://127.0.0.1:$port/rooms/$ROOM"
    done
    if [ $enc = gzip ]; then gunzip -c "$OUT/$app-$enc.raw" > "$OUT/$app-$enc.body"; else cp "$OUT/$app-$enc.raw" "$OUT/$app-$enc.body"; fi
  done
  cmp -s "$OUT/rust-$enc.body" "$OUT/gate-$enc.body" && r=ok || r=bad
  check "room page body byte-identical, Accept-Encoding: $enc ($(wc -c < "$OUT/gate-$enc.body") bytes, sha256 $(sha256sum "$OUT/gate-$enc.body" | cut -c1-16)...)" $r
  names "$OUT/rust-$enc.h" > "$OUT/rust-$enc.names"; names "$OUT/gate-$enc.h" > "$OUT/gate-$enc.names"
  diff -u "$OUT/rust-$enc.names" "$OUT/gate-$enc.names" >/dev/null && r=ok || r=bad
  check "room page header names and order identical, Accept-Encoding: $enc" $r
  [ $r = bad ] && diff -u "$OUT/rust-$enc.names" "$OUT/gate-$enc.names"
done
# a request with an Accept header makes Rails put "vary: Accept,Accept-Encoding" first
for app in rust gate; do
  if [ $app = rust ]; then port=$RUST_PORT; ck=$CR; else port=$GATE_PORT; ck=$CG; fi
  curl -s -D "$OUT/$app-accept.h" -o /dev/null -H "cookie: $ck" -H "accept-encoding: gzip" -H "accept: text/html" -H "host: 127.0.0.1:4390" "http://127.0.0.1:$port/rooms/$ROOM"
done
names "$OUT/rust-accept.h" > "$OUT/rust-accept.names"; names "$OUT/gate-accept.h" > "$OUT/gate-accept.names"
diff -u "$OUT/rust-accept.names" "$OUT/gate-accept.names" >/dev/null && r=ok || r=bad
check "room page header names and order identical, request with an Accept header" $r
echo "--- gate gzip response headers:"; sed 's/\(link: .\{40\}\).*/\1.../' "$OUT/gate-gzip.h"
echo "--- rust gzip response headers:"; sed 's/\(link: .\{40\}\).*/\1.../' "$OUT/rust-gzip.h"

# 20 identical posts to both apps
norm() { sed -E 's/[0-9]{13}/MS/g; s/[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:]{8}Z/ISO/g'; }
for n in $(seq 1 20); do
  body="message%5Bbody%5D=bench%20write%20$n&message%5Bclient_message_id%5D=cmp$n&authenticity_token="
  for app in rust gate; do
    if [ $app = rust ]; then port=$RUST_PORT; ck=$CR; else port=$GATE_PORT; ck=$CG; fi
    code=$(curl -s -o "$OUT/post-$app-$n.raw" -D "$OUT/post-$app-$n.h" -w '%{http_code}' -X POST -H "cookie: $ck" -H 'content-type: application/x-www-form-urlencoded' \
      -H 'accept: text/vnd.turbo-stream.html, text/html, application/xhtml+xml' -H 'x-csrf-token: ' -H 'sec-fetch-site: same-origin' -H 'accept-encoding: gzip' --data "$body" "http://127.0.0.1:$port/rooms/$HQ/messages")
    [ "$code" = 200 ] || { echo "post $n to $app returned $code"; fail=1; }
    gunzip -c "$OUT/post-$app-$n.raw" | norm > "$OUT/post-$app-$n.body"
  done
done
pb=0; ph=0
for n in $(seq 1 20); do
  cmp -s "$OUT/post-rust-$n.body" "$OUT/post-gate-$n.body" || { pb=1; diff "$OUT/post-rust-$n.body" "$OUT/post-gate-$n.body" | head -5; }
  [ "$(names "$OUT/post-rust-$n.h")" = "$(names "$OUT/post-gate-$n.h")" ] || ph=1
done
[ $pb = 0 ] && r=ok || r=bad; check "20 post response bodies identical (timestamps normalized)" $r
[ $ph = 0 ] && r=ok || r=bad; check "20 post response header names and order identical" $r
echo "--- gate post headers:"; cat "$OUT/post-gate-20.h"; echo "--- rust post headers:"; cat "$OUT/post-rust-20.h"
sleep 1
docker stop g4-cmp-rust g4-cmp-gate >/dev/null
echo "--- table comparison (rust vs gate after 20 posts each):"
rn python3 "$HERE/compare_tables.py" $BENCH/g4-cmp-rust/db/production.sqlite3 $BENCH/g4-cmp-gate/db/production.sqlite3 && r=ok || r=bad
check "changed rows in messages, rich texts, memberships, rooms and the search index are equal" $r
rn rm -rf $BENCH/g4-cmp-rust $BENCH/g4-cmp-gate
echo "work files: $OUT"
[ $fail = 0 ] && echo "compare.sh: ALL CHECKS PASSED" || echo "compare.sh: FAILED"
exit $fail
