Median [min-max] over cpp: 1 rep(s), rust: 1 rep(s).

| route | conc | app | req/s | p50 ms | p99 ms | app CPU ms/req |
|---|---:|---|---:|---:|---:|---:|
| messages_page | 1 | cpp | 12,974 | 0.075 | 0.087 | 0.042 |
| messages_page | 1 | rust | 6,529 | 0.15 | 0.28 | 0.14 |
| messages_page | 16 | cpp | 61,390 | 0.25 | 0.61 | 0.053 |
| messages_page | 16 | rust | 29,966 | 0.51 | 1.00 | 0.11 |
| messages_page | 64 | cpp | 61,519 | 0.95 | 2.24 | 0.055 |
| messages_page | 64 | rust | 30,442 | 2.04 | 3.75 | 0.11 |
| search | 1 | cpp | 12,443 | 0.079 | 0.091 | 0.045 |
| search | 1 | rust | 5,301 | 0.19 | 0.22 | 0.16 |
| search | 16 | cpp | 46,075 | 0.33 | 0.57 | 0.057 |
| search | 16 | rust | 25,547 | 0.60 | 1.20 | 0.13 |
| search | 64 | cpp | 41,792 | 1.57 | 2.75 | 0.076 |
| search | 64 | rust | 31,138 | 1.95 | 4.13 | 0.11 |

Errors and non-2xx/3xx responses:
- cpp: none
- rust: none

Average response bytes (first rep): cpp messages_page c=1: 12065; cpp search c=1: 9357; rust messages_page c=1: 16158; rust search c=1: 9766
