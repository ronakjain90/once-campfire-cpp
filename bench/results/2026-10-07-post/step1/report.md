Median [min-max] over cpp: 3 rep(s), rust: 3 rep(s).

| route | conc | app | req/s | p50 ms | p99 ms | app CPU ms/req |
|---|---:|---|---:|---:|---:|---:|
| post_message | 16 | cpp | 7,940 [5,853-8,258] | 1.84 [1.81-1.93] | 4.80 [4.55-10.20] | 0.37 [0.37-0.38] |
| post_message | 16 | rust | 5,542 [5,440-5,568] | 2.70 [2.68-2.73] | 7.86 [7.81-7.92] | 0.48 [0.48-0.49] |
| post_message | 64 | cpp | 10,719 [5,938-11,542] | 5.73 [5.31-10.27] | 13.5 [12.5-25.0] | 0.29 [0.27-0.34] |
| post_message | 64 | rust | 5,176 [4,575-5,393] | 11.3 [10.9-13.6] | 21.9 [21.8-22.3] | 0.49 [0.47-0.58] |

Errors and non-2xx/3xx responses:
- cpp: none
- rust: none

Average response bytes (first rep): cpp post_message c=16: 2096; rust post_message c=16: 1992
