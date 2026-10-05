# Phase 0 gate: result

Date: 2026-10-05
Decision: **go**. Start Phase 1.

## Setup

- Host: Apple M4. Colima VM: Ubuntu, kernel 6.8, arm64, 8 vCPUs, 16 GB.
- App on CPUs 0–3. Load generator (the Rust `loadgen`) on CPUs 4–7. Databases on VM-local ext4.
- Rust: `campfire-rust:app`, built from `once-campfire-rust` at `64f8635`.
- Gate: `campfire-gate:app`, built from `gate/server/` (task G4).
- 3 reps, with the order of the apps changed in each rep. 8 seconds for each cell.
- Command: `gate/bench/run --apps rust=campfire-rust:app,gate=campfire-gate:app --reps 3 --out gate/results/g5`
- Raw data: `gate/results/g5/`. No errors, and all responses were 2xx or 3xx.

## Results

Median [min–max] of 3 reps.

| Route | Clients | Rust req/s | Gate req/s | Gate / Rust | Rust CPU ms/req | Gate CPU ms/req |
|---|---:|---:|---:|---:|---:|---:|
| Room page | 1 | 6,363 [6,198–6,487] | 11,489 [11,463–11,519] | 1.81× | 0.14 | 0.051 |
| Room page | 16 | 27,610 [27,604–29,372] | 59,898 [58,736–60,383] | **2.17×** | 0.12 | 0.054 |
| Room page | 64 | 29,672 [28,364–30,029] | 62,316 [60,778–63,232] | 2.10× | 0.12 | 0.056 |
| Post a message | 1 | 2,729 [2,717–2,747] | 5,267 [5,197–5,366] | 1.93× | 0.41 | 0.16 |
| Post a message | 16 | 7,017 [6,953–7,032] | 12,910 [11,851–13,092] | **1.84×** | 0.39 | 0.17 |
| Post a message | 64 | 6,401 [6,346–7,082] | 14,573 [14,190–14,733] | 2.28× | 0.43 | 0.15 |

The ranges of the two apps do not overlap in any cell.

Pass rule (`plans/cpp-port.md`, section 3): the room page at 16 clients must be at least 1.6×.
The result is 2.17×. The post result, 1.84×, also passes the second rule.

## What the gate does not do

Task G4 lists all differences in `gate/server/README.md`. These differences can change the result:

1. **Post rendering.** The gate fills a captured template. It does not sanitize the rich text or
   render the message partial. In the older Rust profile, these steps used about 39% of the CPU of
   a post, which is about 0.15 ms now.
   - If the C++ app needs the same 0.15 ms, a post costs about 0.32 ms of CPU. The 4 cores then
     limit the app to about 12,500 posts per second: about 1.8× Rust.
   - If the C++ app needs two times as long as Rust (0.30 ms), the limit is about 8,500 posts per
     second: about 1.2× Rust. Thus the post rendering in Phase 5 must be at least as fast as Rust.
2. **Room page cache key.** The key has the user id but not the user row. The real app must add
   the `updated_at` of the user. This adds no SQL, because the session cache has the row.
3. **Smaller gzip output.** The gate compresses the whole page once (20.7 KB). Rust joins gzip
   parts (24.2 KB). The gate sends 15% fewer bytes. This gives a small advantage on loopback.
4. **No page render on a cache miss.** The benchmark does not change the room page, so neither app
   renders it during the run. The real app must render a page on a miss. That cost is not in
   these numbers, for either app.

## Where the gain comes from

- **Room page:** Rust uses 0.12 ms of CPU for each request, and the gate uses 0.054 ms. Both run
  the same 7 SQL reads (the gate skips the session and user reads, as rule 1 allows). The gate
  keys its page cache on row values and sends a gzip body that it keeps. Rust joins its page from
  parts and hashes them on each request.
- **Post:** the group commit, and less CPU for each request (0.17 ms against 0.39 ms).
