# Action Cable fan-out, 2026-10-08

`bench/run` of this repo, 3 interleaved reps, 100 and 1,000 clients in one room. The apps run on
CPUs 0-3, and the load generator runs on CPUs 4-7.

| App | Source |
|---|---|
| `c4` | `fe6376a`: broadcasts coalesce for 4 ms under load (the default) |
| `c8` | the same code with a window of 8 ms |
| `rust` | [basecamp/once-campfire-rust](https://github.com/basecamp/once-campfire-rust) `b7f4af0` |

`env.txt` gives the image digests. The README uses the `c4` rows.

## Output of `bench/report`

Median [min-max] over c4: 3 rep(s), c8: 3 rep(s), rust: 3 rep(s).

| route | conc | app | req/s | p50 ms | p99 ms | app CPU ms/req |
|---|---:|---|---:|---:|---:|---:|

Errors and non-2xx/3xx responses:
- c4: none
- c8: none
- rust: none

Average response bytes (first rep): 

Action Cable fan-out (one post reaches every client):

| clients | app | ready | deliveries/s | post-to-all p50 ms | post-to-all p99 ms | complete / posted |
|---:|---|---:|---:|---:|---:|---|
| 100 | c4 | 100 [100-100] | 4,809 [4,783-4,836] | 3.54 [3.52-3.55] | 5.13 [5.01-5.61] | 71771/71771, 72161/72161, 72554/72554 |
| 100 | c8 | 100 [100-100] | 5,044 [4,944-5,097] | 3.52 [3.46-3.68] | 5.46 [5.17-5.99] | 74210/74210, 75691/75691, 76492/76492 |
| 100 | rust | 100 [100-100] | 2,793 [2,766-2,870] | 4.15 [3.95-4.29] | 6.27 [6.05-7.04] | 43050/43050, 41500/41500, 41901/41901 |
| 1000 | c4 | 1,000 [1,000-1,000] | 534 [521-538] | 13.7 [13.4-13.7] | 17.4 [17.4-36.9] | 7823/7823, 8021/8021, 8074/8074 |
| 1000 | c8 | 1,000 [1,000-1,000] | 576 [554-592] | 13.4 [13.2-14.6] | 19.5 [16.3-51.6] | 8656/8656, 8882/8882, 8323/8323 |
| 1000 | rust | 1,000 [1,000-1,000] | 496 [494-498] | 16.3 [15.0-16.8] | 20.0 [19.5-20.2] | 7447/7447, 7410/7410, 7475/7475 |
