#!/usr/bin/env bash
# Runs libFuzzer targets one after the other for N seconds each (build them with tools/fuzz-build.sh).
# Usage: tools/fuzz-run.sh <seconds> <target>...   (a target is the file name, e.g. fuzz_net_misc)
# Corpus: the volume cfcpp-build-H1 at /build/fuzz-corpus/<target>; seeds: tests/fuzz/seeds/<target>/;
# dictionary: tests/fuzz/dict/<target>.dict. Logs: $LOGDIR (default: ../h1-logs/fuzz). A crash file
# goes to /build/fuzz-artifacts and is copied to $LOGDIR.
set -uo pipefail
cd "$(dirname "$0")/.."
secs=$1; shift
logdir=${LOGDIR:-$(cd .. && pwd)/h1-logs/fuzz}
mkdir -p "$logdir"
for t in "$@"; do
  # Container name: bin/dev uses h1-dev-<pid>, so two lanes can run at once.
  bin/dev run bash -lc '
    t=$1; secs=$2
    bin=$(ls /build/fuzz/src/*/fuzz/$t /build/fuzz/src/*/$t 2>/dev/null | head -1)
    mkdir -p /build/fuzz-corpus/$t /build/fuzz-artifacts
    args=(-max_total_time=$secs -timeout=20 -rss_limit_mb=4096 -print_final_stats=1 -max_len=${MAXLEN:-8192}
          -artifact_prefix=/build/fuzz-artifacts/$t- -ignore_ooms=0)
    [ -f tests/fuzz/dict/$t.dict ] && args+=(-dict=tests/fuzz/dict/$t.dict)
    seeds=; [ -d tests/fuzz/seeds/$t ] && seeds=tests/fuzz/seeds/$t
    export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1
    "$bin" "${args[@]}" /build/fuzz-corpus/$t $seeds' x "$t" "$secs" > "$logdir/$t.log" 2>&1
  echo "$t exit=$? $(grep -c '' "$logdir/$t.log") lines" >> "$logdir/summary.txt"
done
