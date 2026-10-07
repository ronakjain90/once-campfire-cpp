Median [min-max] over cpp: 2 rep(s), rust: 2 rep(s).

| route | conc | app | req/s | p50 ms | p99 ms | app CPU ms/req |
|---|---:|---|---:|---:|---:|---:|
| post_message | 16 | cpp | 5,801 [4,358-7,244] | 2.71 [2.04-3.37] | 8.14 [6.30-9.97] | 0.52 [0.41-0.62] |
| post_message | 16 | rust | 4,492 [4,434-4,550] | 3.31 [3.29-3.34] | 9.09 [8.75-9.43] | 0.57 [0.55-0.60] |
| post_message | 64 | cpp | 8,707 [7,818-9,597] | 6.87 [6.42-7.31] | 17.2 [14.4-19.9] | 0.36 [0.33-0.39] |
| post_message | 64 | rust | 4,195 [3,962-4,428] | 14.3 [13.7-14.8] | 31.8 [30.5-33.1] | 0.64 [0.61-0.68] |

Errors and non-2xx/3xx responses:
- cpp: none
- rust: none

Average response bytes (first rep): cpp post_message c=16: 1985; rust post_message c=16: 1992
