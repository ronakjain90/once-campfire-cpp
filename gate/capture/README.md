# G3: capture of what the Rust app does per request

Result: `summary.md`. Raw data: `raw/`. Table dumps: `tables/`.

## Rust trace change
Branch `gate-sql-trace` in the worktree `once-campfire-rust-trace` (not pushed). The diff is `rust-trace.diff`. If the env var `CAMPFIRE_SQL_TRACE` is set, each connection writes `SQLTRACE start|end <reader|writer> <thread> ...` lines to stderr. Without it, the app does not change.

Build the image (from the host):

    cd /Volumes/ExternalHD/Code/AI/once-campfire/once-campfire-rust-trace
    git submodule update --init
    docker build -t campfire-rust:trace .

## Reproduce
Needs the images `campfire-rust:trace` and `campfire-bench-runner`, `tcpdump` in the VM, and bash. Nothing else may use ports 4490 and 4491.

1. `./run-capture.sh`. It does these steps:
   - `setup-app.sh` copies the seed to `/var/lib/campfire-bench/g3` and sets push and webhook endpoints to `127.0.0.1:9` (as G2). It starts `g3-app` (CPUs 0-3, `--user 1000:1000`, G2 env, `CAMPFIRE_SQL_TRACE=1`).
   - It starts `tcpdump` on port 9 in the VM.
   - `capture.py` runs in a `--network host` container and sends the requests. It marks each request with `/up?mark=N` before and after it, and records the time window of each request in `raw/windows.json`.
   - It saves `docker logs -t g3-app` as `raw/app.log`, and copies the database to `/var/lib/campfire-bench/g3-snap`.
2. `python3 split.py` splits `raw/app.log` into `raw/sql/<request>.txt`, by time window.
3. `python3 gen-summary.py` parses these files into `raw/sql-statements.json`. It checks that the 11 posts run the same statements.
4. Dump the changed rows (the seed copy is made inside the container):

       docker run --rm --network none -v $PWD:/out -v <seed>/db:/seed:ro -v /var/lib/campfire-bench/g3-snap:/snap:ro campfire-bench-runner sh -c 'mkdir -p /tmp/s && cp /seed/* /tmp/s/ && python3 /out/dump-tables.py /tmp/s/production.sqlite3 /snap/production.sqlite3 /out/tables'
5. `python3 make-summary.py` writes `summary.md`.

`bin/loadgen` is the load generator binary from the runner image. `raw/loadgen-wire.sh` shows how the bytes of a real `loadgen` post were captured (`raw/loadgen-wire.txt`).
