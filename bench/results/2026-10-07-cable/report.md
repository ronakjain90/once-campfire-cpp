Median [min-max] over cpp: 3 rep(s), rust: 3 rep(s).

| route | conc | app | req/s | p50 ms | p99 ms | app CPU ms/req |
|---|---:|---|---:|---:|---:|---:|

Errors and non-2xx/3xx responses:
- cpp: none
- rust: none

Average response bytes (first rep): 

Action Cable fan-out (one post reaches every client):

| clients | app | ready | deliveries/s | post-to-all p50 ms | post-to-all p99 ms | complete / posted |
|---:|---|---:|---:|---:|---:|---|
| 100 | cpp | 100 [100-100] | 3,592 [3,552-3,688] | 3.52 [3.33-3.79] | 5.03 [4.92-5.08] | 55327/55327, 53878/53878, 53281/53281 |
| 100 | rust | 100 [100-100] | 3,098 [2,815-3,162] | 4.16 [3.87-4.17] | 5.32 [5.17-7.29] | 46475/46475, 47437/47437, 42224/42224 |
| 1000 | cpp | 1,000 [1,000-1,000] | 479 [459-496] | 14.1 [13.8-16.1] | 18.2 [16.4-20.4] | 7436/7436, 7195/7195, 6889/6889 |
| 1000 | rust | 1,000 [1,000-1,000] | 546 [503-550] | 15.9 [14.5-17.7] | 20.3 [19.9-24.4] | 8195/8195, 8253/8253, 7546/7546 |
