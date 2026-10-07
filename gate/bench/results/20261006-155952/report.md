Median [min-max] over cpp: 2 rep(s), rust: 2 rep(s).

| route | conc | app | req/s | p50 ms | p99 ms | app CPU ms/req |
|---|---:|---|---:|---:|---:|---:|
| post_message | 1 | cpp | 4,327 [4,320-4,334] | 0.19 [0.19-0.19] | 0.76 [0.73-0.80] | 0.19 [0.19-0.19] |
| post_message | 1 | rust | 2,614 [2,591-2,636] | 0.37 [0.36-0.37] | 0.67 [0.65-0.69] | 0.42 [0.42-0.43] |
| post_message | 16 | cpp | 11,528 [6,164-16,892] | 1.52 [0.84-2.19] | 5.78 [2.70-8.86] | 0.18 [0.13-0.23] |
| post_message | 16 | rust | 5,758 [4,888-6,629] | 2.29 [2.21-2.37] | 9.98 [7.39-12.57] | 0.44 [0.42-0.46] |
| post_message | 64 | cpp | 11,059 [9,075-13,042] | 5.00 [3.61-6.39] | 17.5 [15.2-19.9] | 0.16 [0.14-0.18] |
| post_message | 64 | rust | 5,460 [4,680-6,240] | 10.5 [9.1-11.9] | 28.4 [21.4-35.5] | 0.45 [0.42-0.49] |

Errors and non-2xx/3xx responses:
- cpp: none
- rust: none

Average response bytes (first rep): cpp post_message c=1: 195; rust post_message c=1: 1993
