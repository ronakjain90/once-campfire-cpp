# Shared verification harness, 2026-10-08, branch `perf-hit-path`

The run used [basecamp/once-campfire-verification](https://github.com/basecamp/once-campfire-verification)
at revision `4d1da88`. Every measured response must match its route contract, and every acknowledged
post must match its persisted row, body, room and search-index entry. Any failure stops the run.

## Results

Medians of 3 alternating rounds, 16 concurrent clients, 2-second warmups and 8-second samples.

| Route | C++ | Rust | C++ advantage |
|---|---:|---:|---:|
| Room page | 147,595 req/s | 48,309 req/s | **3.06×** |
| Messages page | 144,925 req/s | 45,430 req/s | **3.19×** |
| Sidebar | 156,281 req/s | 53,989 req/s | **2.89×** |
| Search | 161,070 req/s | 53,888 req/s | **2.99×** |
| Post a message | 16,073 req/s | 6,199 req/s | **2.59×** |

| Round | C++ room | C++ post | Rust room | Rust post |
|---|---:|---:|---:|---:|
| 1 | 149,403 | 15,759 | 48,466 | 6,241 |
| 2 | 143,524 | 16,073 | 48,309 | 6,199 |
| 3 | 147,595 | 16,313 | 47,909 | 6,169 |

Validated responses over the 3 rounds: 31,916,779 for C++ and 14,166,745 for Rust, with zero
request errors and zero invalid responses. The write audit verified 449,631 acknowledged C++ posts
and 180,230 acknowledged Rust posts. The per-round files (`cpp-N.json`, `rust-N.json`) and
`summary.json` are the output of the harness. The raw samples are not kept.

The previous run ([2026-10-08-verification](../2026-10-08-verification/report.md)) used the same
Rust image on the same host, but harness revision `7b2dbc7`. Against that run, the C++ app is
faster by these amounts: room page 1.44×, messages page 1.23×, sidebar 1.46×, search 1.38×, post
1.17×.

## Reads while posts arrive

A second run (`20261008-155315-1`, profile `mixed-read-write-v1`) measured the same reads while one
writer posted 10 messages per second to a different room. It tested `64fb4fd`, five commits before
`79c4b55` on this branch, against the same Rust image. The files are in `mixed/`.

| Route | C++ | Rust | C++ advantage |
|---|---:|---:|---:|
| Room page | 144,549 req/s | 46,133 req/s | **3.13×** |
| Messages page | 129,208 req/s | 43,512 req/s | **2.97×** |
| Sidebar | 146,958 req/s | 53,765 req/s | **2.73×** |
| Search | 156,940 req/s | 52,919 req/s | **2.97×** |

All 13,983,531 C++ reads and all 4,707,976 Rust reads were valid. The write audit verified all
1,200 C++ posts and all 1,196 Rust posts of the writer.

## Sources

| App | Source | Image |
|---|---|---|
| C++ | `79c4b55` on `perf-hit-path` (clean) | `sha256:4a08238b…` |
| Rust | [basecamp/once-campfire-rust](https://github.com/basecamp/once-campfire-rust) `b7f4af0` | `sha256:b1a974fe…` |

The next commit, `fe6376a`, changes only the Action Cable broadcasts. The harness does not measure
Action Cable, so these numbers also apply to `fe6376a`.

## Setup

- Apple M4 Mac (10 cores, 32 GB). Docker runs in a Colima VM with 8 vCPUs and 16 GB.
- The apps run on CPUs 0-3, and the load generator runs on CPUs 4-7.
- The harness runs in a Linux runner container, as in the previous run.
- The databases of the apps are on the ext4 disk of the VM.
- The harness knows the C++ app since `c89aa50`. This run needs no local patch.

The absolute numbers depend on the host. Compare the ratios of one run only.
