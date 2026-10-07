Median [min-max] over cpp: 2 rep(s), rust: 2 rep(s).

| route | conc | app | req/s | p50 ms | p99 ms | app CPU ms/req |
|---|---:|---|---:|---:|---:|---:|
| post_message | 16 | cpp | 13,732 [13,436-14,029] | 1.07 [1.06-1.08] | 3.39 [2.52-4.25] | 0.24 [0.23-0.24] |
| post_message | 16 | rust | 6,748 [6,687-6,810] | 2.23 [2.21-2.24] | 7.05 [6.97-7.13] | 0.41 [0.40-0.41] |
| post_message | 64 | cpp | 16,190 [15,294-17,086] | 3.54 [3.43-3.65] | 11.6 [10.0-13.2] | 0.21 [0.19-0.22] |
| post_message | 64 | rust | 6,681 [6,494-6,868] | 9.04 [8.99-9.09] | 19.5 [15.1-24.0] | 0.41 [0.40-0.42] |

Errors and non-2xx/3xx responses:
- cpp: none
- rust: none

Average response bytes (first rep): cpp post_message c=16: 2097; rust post_message c=16: 1992
