Median [min-max] over rust: 1 rep(s).

| route | conc | app | req/s | p50 ms | p99 ms | app CPU ms/req |
|---|---:|---|---:|---:|---:|---:|
| room_show | 1 | rust | 6,514 | 0.15 | 0.21 | 0.14 |
| room_show | 16 | rust | 17,891 | 0.61 | 4.49 | 0.14 |
| room_show | 64 | rust | 27,446 | 2.21 | 4.94 | 0.12 |
| post_message | 1 | rust | 2,578 | 0.37 | 0.64 | 0.42 |
| post_message | 16 | rust | 6,159 | 2.40 | 7.23 | 0.36 |
| post_message | 64 | rust | 6,584 | 9.38 | 16.1 | 0.34 |

Errors and non-2xx/3xx responses:
- rust: none

Average response bytes (first rep): rust room_show c=1: 24231; rust post_message c=1: 1992
