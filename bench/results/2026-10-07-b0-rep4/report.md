Median [min-max] over cpp: 1 rep(s), rust: 1 rep(s).

| route | conc | app | req/s | p50 ms | p99 ms | app CPU ms/req |
|---|---:|---|---:|---:|---:|---:|
| sidebar | 1 | cpp | 12,158 | 0.080 | 0.099 | 0.047 |
| sidebar | 1 | rust | 5,640 | 0.17 | 0.35 | 0.16 |
| sidebar | 16 | cpp | 67,072 | 0.22 | 0.52 | 0.051 |
| sidebar | 16 | rust | 24,044 | 0.60 | 1.77 | 0.14 |
| sidebar | 64 | cpp | 67,677 | 0.92 | 1.49 | 0.050 |
| sidebar | 64 | rust | 21,684 | 2.60 | 8.21 | 0.16 |
| post_message | 1 | cpp | 4,529 | 0.20 | 0.33 | 0.29 |
| post_message | 1 | rust | 2,730 | 0.36 | 0.53 | 0.41 |
| post_message | 16 | cpp | 11,699 | 1.28 | 2.95 | 0.24 |
| post_message | 16 | rust | 7,068 | 2.14 | 6.84 | 0.39 |
| post_message | 64 | cpp | 15,076 | 4.14 | 8.11 | 0.20 |
| post_message | 64 | rust | 7,182 | 8.62 | 14.3 | 0.38 |
| room_show | 1 | cpp | 11,876 | 0.082 | 0.096 | 0.048 |
| room_show | 1 | rust | 6,387 | 0.15 | 0.18 | 0.14 |
| room_show | 16 | cpp | 37,746 | 0.37 | 1.11 | 0.085 |
| room_show | 16 | rust | 25,779 | 0.59 | 1.23 | 0.13 |
| room_show | 64 | cpp | 39,906 | 1.56 | 2.87 | 0.081 |
| room_show | 64 | rust | 26,346 | 2.35 | 4.47 | 0.13 |

Errors and non-2xx/3xx responses:
- cpp: none
- rust: none

Average response bytes (first rep): cpp sidebar c=1: 5826; cpp post_message c=1: 2097; cpp room_show c=1: 20658; rust sidebar c=1: 5910; rust post_message c=1: 1993; rust room_show c=1: 24231
