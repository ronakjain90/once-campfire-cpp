# Campfire in C++

This repository is a C++ port of [Campfire](https://github.com/basecamp/once-campfire). It has two
goals:

1. Feature parity with the Rust port (`once-campfire-rust`). The port uses the same database,
   storage layout, cookies, URLs and environment variables, so an existing install upgrades with no
   data migration.
2. At least 1.5 times the throughput of the Rust port on the same host.

The plan is in `plans/cpp-port.md`. The status of each task is in `plans/tasks.md`.

## Status

All feature areas are on `main`. The diff sweep sends 689 requests to the Rust app and to the C++
app. All responses are equal, byte for byte, in all 9 areas.

Open work:

- Parity checks against Rails: the Playwright harness, and database and cookie compatibility in
  both directions.
- A full benchmark of all routes on a quiet host.
- Performance of the Action Cable fan-out with 1,000 clients.

## Build and test

Run all commands through `bin/dev`. The script runs them in the `campfire-cpp-dev` Docker image.

```
bin/dev build release
bin/dev test release
```

The presets are `release`, `asan` and `tsan`. `AGENTS.md` lists all commands.

Build the production image:

```
docker build -t campfire-cpp:app -f docker/Dockerfile .
```

The image is a drop-in replacement for `campfire-rust:app`: it has the same user, ports, storage
layout and entrypoint.

## Benchmarks

### Setup

- Host: Apple M4. Colima VM: Ubuntu, kernel 6.8, arm64, 8 vCPUs.
- The app runs on CPUs 0–3. The load generator (`loadgen` from the Rust repo) runs on CPUs 4–7.
- Each cell runs for 8 seconds. In a run with more than 1 rep, the order of the apps changes in
  each rep, and the table shows the median. The report of each run shows the ranges.
- No run had errors. All responses were 2xx or 3xx.

### HTTP routes

Run: `bench/results/2026-10-07-b0-rep4/report.md` (2026-10-07, quiet host, 1 rep).

| Route | Clients | Rust req/s | C++ req/s | C++ / Rust |
|---|---:|---:|---:|---:|
| Room page | 1 | 6,387 | 11,876 | 1.86× |
| Room page | 16 | 25,779 | 37,746 | **1.46×** |
| Room page | 64 | 26,346 | 39,906 | 1.51× |
| Sidebar | 1 | 5,640 | 12,158 | 2.16× |
| Sidebar | 16 | 24,044 | 67,072 | 2.79× |
| Sidebar | 64 | 21,684 | 67,677 | 3.12× |
| Post a message | 1 | 2,730 | 4,529 | 1.66× |
| Post a message | 16 | 7,068 | 11,699 | 1.66× |
| Post a message | 64 | 7,182 | 15,076 | 2.10× |

The C++ app also uses less CPU for each request: 0.085 ms against 0.13 ms for the room page at 16
clients.

This run has 1 rep, not 3. It used the same images as the run in
`bench/results/2026-10-07-b0-final`. Do not use that earlier run: fuzz runs used the host at that
time, and its numbers are too low for both apps. For example, Rust did between 5,296 and 17,702
req/s on the room page at 16 clients, and 25,779 req/s on a quiet host.

The room page from the C++ app has an average of 20,658 bytes, and from the Rust app 24,231 bytes.
The cause of this difference is not known yet. Thus the room page ratios can change.

### Action Cable fan-out

Run: `bench/results/2026-10-07-cable/report.md` (2026-10-07, quiet host).

Each post goes to every connected client. "Deliveries/s" is the number of posts per second that
reached all clients. "p50" is the median time from a post to its delivery to the last client.

| Clients | Rust deliveries/s | C++ deliveries/s | C++ / Rust | Rust p50 | C++ p50 |
|---:|---:|---:|---:|---:|---:|
| 100 | 3,098 | 3,592 | 1.16× | 4.2 ms | 3.5 ms |
| 1,000 | 546 | 479 | **0.88×** | 15.9 ms | 14.1 ms |

With 1,000 clients, the C++ app has a lower throughput than the Rust app, but a lower latency.
This is open work.

### Phase 0 gate

Run: `bench/results/gate/report.md` (2026-10-05, quiet host).

Before the port started, a small C++ server did the same work as the Rust app for two routes. The
result decided if the port could reach the goal.

| Route | Clients | Rust req/s | Gate server req/s | Gate / Rust |
|---|---:|---:|---:|---:|
| Room page | 16 | 27,610 | 59,898 | 2.17× |
| Post a message | 16 | 7,017 | 12,910 | 1.84× |

The gate server is not the full app. These numbers show the best possible result, not the result
of the port.

### Run a benchmark

```
gate/bench/run --apps rust=campfire-rust:app,cpp=campfire-cpp:app \
  --routes room_show,sidebar,post_message --reps 3 --out bench/results/<name>
```

Add `--cable "100 1000"` to run the Action Cable fan-out. Set `--routes ""` to run only the
fan-out. `gate/bench/README.md` lists all options.

Stop all other work on the host before a run. Other load changes the results.
