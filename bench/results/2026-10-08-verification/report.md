# Shared verification harness, 2026-10-08

The run used [basecamp/once-campfire-verification](https://github.com/basecamp/once-campfire-verification)
at revision `7b2dbc7`. Every measured response must match its route contract, and every acknowledged
post must match its persisted row, body, room and search-index entry. Any failure stops the run.

## Results

Medians of 3 alternating rounds, 16 concurrent clients, 2-second warmups and 8-second samples.

| Route | C++ | Rust | C++ advantage |
|---|---:|---:|---:|
| Room page | 102,650 req/s | 47,967 req/s | **2.14×** |
| Messages page | 117,679 req/s | 44,983 req/s | **2.62×** |
| Sidebar | 107,246 req/s | 52,494 req/s | **2.04×** |
| Search | 116,777 req/s | 53,167 req/s | **2.20×** |
| Post a message | 13,718 req/s | 6,075 req/s | **2.26×** |

| Round | C++ room | C++ post | Rust room | Rust post |
|---|---:|---:|---:|---:|
| 1 | 102,650 | 13,733 | 47,736 | 5,879 |
| 2 | 100,233 | 13,718 | 47,967 | 6,075 |
| 3 | 107,689 | 13,706 | 47,984 | 6,142 |

Validated responses over the 3 rounds: 28,026,422 for C++ and 13,758,527 for Rust, with zero
request errors and zero invalid responses. The write audit verified 386,877 acknowledged C++ posts
and 178,582 acknowledged Rust posts. The per-round files (`cpp-N.json`, `rust-N.json`) and
`summary.json` are the output of the harness. The raw samples are not kept.

## Sources

| App | Source | Image |
|---|---|---|
| C++ | `6b57baf`: `main` at `1cb6617`, with `4044152` (#7) and `2cff76b` (#8) | `sha256:77268c13…` |
| Rust | [basecamp/once-campfire-rust](https://github.com/basecamp/once-campfire-rust) `b7f4af0` | `sha256:b1a974fe…` |

Both apps have a page cache for signed-in pages. The Rust port added its cache in `d09811c`.

## Setup

- Apple M4 Mac (10 cores, 32 GB). Docker runs in a Colima VM with 8 vCPUs and 16 GB.
- The apps run on CPUs 0-3, and the load generator runs on CPUs 4-7. The published harness
  results use CPUs 8-11 and 12-15 of a larger host.
- The harness runs in a Linux runner container (Rust 1.98.1, Ruby, SQLite, FFmpeg and the Docker
  CLI), as user 1000 with the group of the Docker socket.
- The databases of the apps are on the ext4 disk of the VM. `tmp/bench/runtime` in the harness
  checkout is a symbolic link to a folder on that disk. On the virtiofs mount of the Mac folder,
  SQLite writes are about 10 times slower.
- The harness does not know the C++ app. `harness-cpp-app.patch` adds it as `cpp`. The patch
  changes only the list of apps and the topology record.

The absolute numbers depend on the host. Compare the ratios of one run only.
