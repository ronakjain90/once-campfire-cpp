# Gate benchmark harness (task G2)

The harness runs the Rust load generator against app images in the Colima VM. The app runs on
CPUs 0-3. The load generator runs on CPUs 4-7.

## Run

    gate/bench/run --apps rust=campfire-rust:app --reps 1
    gate/bench/run --apps rust=campfire-rust:app,gate=campfire-gate:app --reps 3

Options: `--apps name=image,...` (default `rust=campfire-rust:app`), `--reps` (default 3),
`--routes` (default `room_show,post_message`), `--secs` (default 8), `--concs` (default `"1 16 64"`),
`--cable "100 1000"` (run the Action Cable fan-out with these client counts; default none),
`--out DIR` (default `gate/bench/results/<time>`), `--rebuild` (build the runner image again).
Set `--routes ""` to run only the fan-out. `CABLE_TPUT_SECS` (15) and `CABLE_POSTERS` (4) set the
throughput phase of the fan-out, as in `once-campfire-rust/bench/run`.

Routes: `room_show`, `messages_page`, `sidebar`, `search`, `avatar`, `static_css`, `up`,
`post_message`. They are the routes of `once-campfire-rust/bench/run`.

Use bash 3.2 or later on the host. The script builds the `campfire-bench-runner` image on the first
run (the Dockerfile builds the load generator from `once-campfire-rust/bench/loadgen`).
Do not run two harnesses at the same time: both use port 4390 (change it with `PORT`).

## What each rep does

1. Copy the seed to `/var/lib/campfire-bench/<app>-<rep>-<pid>` (VM ext4 disk, not virtiofs).
2. Set push endpoints and webhook URLs to `127.0.0.1:9`.
3. Start the app container `g2-app-4390` (`--network host`, `--cpuset-cpus 0-3`, the environment of
   `bench/run` `env_args`, `HTTP_PORT=4390`, `TARGET_PORT=4391`). Wait for `/up`.
4. `loadgen login`, `loadgen scrape`. For each route: a 2-second warm-up at 4 clients, then
   `--secs` seconds at each concurrency. The app CPU time is the change of `usage_usec` in the
   container `cpu.stat` during the cell.
5. Stop the container. Delete the directory.

The order of the apps alternates between reps.

## Output (in `--out`)

- `<app>-<rep>.json`: the loadgen result of each cell, with `app_cpu_usec` and `requests`.
- `report.md`: output of `gate/bench/report DIR` (median and range over reps).
- `pin.log`: `Cpus_allowed_list` of the app (`/proc/1/status` in its container) and of the load
  generator, and the file system type of the database directory.
- `env.txt`: the image ids and settings.
