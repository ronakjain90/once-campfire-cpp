# Benchmark harness

The harness runs the load generator of the Rust port against app images in Docker. It compares
the C++ app with the Rust app on the same host, with the same seed data. The app runs on CPUs 0-3.
The load generator runs on CPUs 4-7.

## Requirements

- A Docker host with 8 CPUs, cgroup v2 and 10 GB of free space for Docker. A Linux host works.
  On a Mac, use a VM such as [Colima](https://github.com/abiosoft/colima).
- Bash 3.2 or later on the host.
- A clone of the [Rust port](https://github.com/basecamp/once-campfire-rust) next to this repo:

      parent/
        once-campfire-cpp/     this repo
        once-campfire-rust/    the Rust port

  To use a different parent folder, set `CAMPFIRE_WORKSPACE` to it.

## Setup

Do these steps one time.

1. If you use Colima, give the VM 8 CPUs, and share the parent folder with the VM. The VM must see
   the folder at the same path, and it must be writable. For example, in
   `~/.colima/default/colima.yaml`:

       cpu: 8
       memory: 16
       mounts:
         - location: /path/to/parent
           writable: true

   Then restart the VM: `colima stop` and `colima start`.

2. In `once-campfire-rust`, build the seed data and the Rust image. The seed needs the Rails app
   in the `reference/` submodule.

       cd ../once-campfire-rust
       git submodule update --init
       parity/bin/reference build
       parity/bin/seed build default
       parity/bin/candidate build

   The seed goes to `parity/.seed/default`. `parity/bin/candidate build` makes the image
   `campfire-rust:app`.

3. In this repo, build the C++ image:

       docker build -t campfire-cpp -f docker/Dockerfile .

The first run of `bench/run` builds the runner image `campfire-bench-runner`. That image has the
load generator, which is built from `once-campfire-rust/bench/loadgen`.

## Run

Stop all other work on the host before a run. Other work makes the numbers lower and less
stable.

    bench/run --apps rust=campfire-rust:app,cpp=campfire-cpp --reps 3

The README numbers use this command:

    bench/run --apps rust=campfire-rust:app,cpp=campfire-cpp --reps 3 \
      --routes room_show,messages_page,sidebar,search,post_message --concs "16 64 100"

A run of this command takes about 20 minutes. Compare the apps only with the ratios of one run.
The numbers of two runs, or of two hosts, are not comparable.

Before the first rep, the script checks the setup. If a part is missing, it stops and tells you
what to do.

| Option | Default | Meaning |
|---|---|---|
| `--apps name=image,...` | `rust=campfire-rust:app` | The apps to compare |
| `--reps N` | 3 | The number of reps. The order of the apps alternates between reps. |
| `--routes a,b` | `room_show,post_message` | The routes. `""` runs only the fan-out. |
| `--secs N` | 8 | The duration of each measurement, in seconds |
| `--concs "a b"` | `"1 16 64"` | The numbers of concurrent clients |
| `--cable "a b"` | none | Run the Action Cable fan-out with these numbers of clients |
| `--out DIR` | `bench/results/<time>` | The output folder. It must be in the parent folder. |
| `--rebuild` | | Build the runner image again |

Routes: `room_show`, `messages_page`, `sidebar`, `search`, `avatar`, `static_css`, `up`,
`post_message`. They are the routes of `once-campfire-rust/bench/run`.

| Variable | Default | Meaning |
|---|---|---|
| `SERVER_CPUS` | `0-3` | The CPUs of the app |
| `LOADGEN_CPUS` | `4-7` | The CPUs of the load generator |
| `PORT` | 4390 | The port of the app. Two harnesses cannot use the same port. |
| `CABLE_TPUT_SECS` | 15 | The duration of the throughput phase of the fan-out |
| `CABLE_POSTERS` | 4 | The number of posters in the throughput phase |
| `BENCH_APP_ENV` | | More environment for the app: `NAME=value;NAME=value` |
| `BENCH_SECCOMP_UNCONFINED` | | Set it to 1 for a TSan image |

## What each rep does

1. Copy the seed to `/var/lib/campfire-bench/<app>-<rep>-<pid>` on the Docker host.
2. Set the push endpoints and the webhook URLs to `127.0.0.1:9`.
3. Start the app container `campfire-bench-app-4390` (`--network host`, `--cpuset-cpus 0-3`, the
   environment of `env_args` in `bench/run`, `HTTP_PORT=4390`, `TARGET_PORT=4391`). Wait for
   `/up`.
4. Run `loadgen login` and `loadgen scrape`. For each route, run a 2-second warm-up with 4
   clients. Then run `--secs` seconds for each number of clients. The app CPU time is the change
   of `usage_usec` in the `cpu.stat` file of the container during the measurement.
5. Stop the container. Delete the copy of the seed.

## Output (in `--out`)

- `<app>-<rep>.json`: the result of each measurement, with `app_cpu_usec` and `requests`.
- `report.md`: the output of `bench/report DIR`, with the median and the range over the reps.
- `pin.log`: the CPUs of the app and of the load generator, and the file system of the database.
- `env.txt`: the image ids and the settings.
- `<app>-<rep>.applog`: the last 100 MB of the log of the app. Git does not keep these files.

## Problems

- **The Docker VM stops during a run.** Make sure that the Docker disk has free space:
  `colima ssh -- df -h /var/lib/docker`. Delete images and volumes that you do not use. The
  harness keeps at most 100 MB of each app log, but other data can fill the disk.
- **The numbers change much between reps.** Other work runs on the host. Stop it and run again.
- **"Docker does not see ..."**: share the parent folder with the VM (Setup, step 1).
