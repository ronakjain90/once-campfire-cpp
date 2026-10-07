Median [min-max] over cpp: 2 rep(s), rust: 2 rep(s).

| route | conc | app | req/s | p50 ms | p99 ms | app CPU ms/req |
|---|---:|---|---:|---:|---:|---:|
| sidebar | 16 | cpp | 22,298 [17,010-27,585] | 0.53 [0.51-0.56] | 3.39 [1.42-5.37] | 0.12 [0.11-0.12] |
| sidebar | 16 | rust | 17,100 [7,948-26,252] | 1.02 [0.52-1.52] | 5.42 [1.81-9.03] | 0.14 [0.11-0.18] |
| sidebar | 64 | cpp | 24,631 [22,157-27,104] | 2.41 [2.31-2.52] | 6.36 [4.63-8.09] | 0.12 [0.11-0.12] |
| sidebar | 64 | rust | 16,241 [8,798-23,684] | 4.53 [2.54-6.52] | 12.9 [6.1-19.7] | 0.16 [0.13-0.18] |

Errors and non-2xx/3xx responses:
- cpp: none
- rust: none

Average response bytes (first rep): cpp sidebar c=16: 5826; rust sidebar c=16: 5910
