Median [min-max] over gate: 1 rep(s), rust: 1 rep(s).

| route | conc | app | req/s | p50 ms | p99 ms | app CPU ms/req |
|---|---:|---|---:|---:|---:|---:|
| room_show | 1 | gate | 11,415 | 0.086 | 0.098 | 0.051 |
| room_show | 1 | rust | 6,252 | 0.16 | 0.19 | 0.14 |
| room_show | 16 | gate | 62,717 | 0.23 | 0.61 | 0.056 |
| room_show | 16 | rust | 28,800 | 0.53 | 1.09 | 0.12 |
| room_show | 64 | gate | 62,885 | 0.90 | 2.54 | 0.056 |
| room_show | 64 | rust | 29,230 | 2.11 | 4.07 | 0.12 |
| post_message | 1 | gate | 5,373 | 0.17 | 0.30 | 0.16 |
| post_message | 1 | rust | 2,534 | 0.37 | 0.72 | 0.44 |
| post_message | 16 | gate | 13,172 | 1.10 | 4.20 | 0.17 |
| post_message | 16 | rust | 6,804 | 2.16 | 7.01 | 0.40 |
| post_message | 64 | gate | 14,859 | 4.10 | 8.81 | 0.15 |
| post_message | 64 | rust | 7,006 | 8.71 | 14.1 | 0.39 |

Errors and non-2xx/3xx responses:
- gate: none
- rust: none

Average response bytes (first rep): gate room_show c=1: 20658; gate post_message c=1: 1987; rust room_show c=1: 24231; rust post_message c=1: 1993
