# Shared verification harness, 2026-10-09

The runs used [basecamp/once-campfire-verification](https://github.com/basecamp/once-campfire-verification)
at revision `4d1da88`, on the C++ source `00e2349`. The harness recorded `decb7e1`: the same tree,
before a rebase onto `main`. This source has the fixes of the security and cache checks
([report](../2026-10-09-controls/report.md)). The setup is the setup of the
[previous run](../2026-10-08-hit-path/report.md).

## Results

Medians of 3 alternating rounds, 16 concurrent clients.

| Route | C++ | Rust | C++ advantage |
|---|---:|---:|---:|
| Room page | 147,675 req/s | 46,292 req/s | **3.19×** |
| Messages page | 138,058 req/s | 44,643 req/s | **3.09×** |
| Sidebar | 150,161 req/s | 52,905 req/s | **2.84×** |
| Search | 158,351 req/s | 52,562 req/s | **3.01×** |
| Post a message | 15,590 req/s | 6,122 req/s | **2.55×** |

All 31,428,083 C++ responses and all 13,643,694 Rust responses were valid. The write audit verified
434,397 C++ posts and 177,295 Rust posts. The files are in `standard/`.

## Reads while posts arrive

One writer posts 10 messages per second to a different room. Each post is a commit, so it changes
the commit epoch: the next page of each kind renders its message fragments again.

| Route | C++ | Rust | C++ advantage |
|---|---:|---:|---:|
| Room page | 140,772 req/s | 43,156 req/s | **3.26×** |
| Messages page | 143,199 req/s | 42,492 req/s | **3.37×** |
| Sidebar | 155,290 req/s | 52,998 req/s | **2.93×** |
| Search | 154,744 req/s | 50,587 req/s | **3.06×** |

All 14,245,281 C++ reads and all 4,533,307 Rust reads were valid. The write audit verified all
1,196 C++ posts and all 1,199 Rust posts of the writer. The files are in `mixed/`.

## Sources

| App | Source | Image |
|---|---|---|
| C++ | `00e2349`, clean (measured as `decb7e1`, the same tree before a rebase) | `sha256:bb7600c3…` |
| Rust | [basecamp/once-campfire-rust](https://github.com/basecamp/once-campfire-rust) `b7f4af0` | `sha256:b1a974fe…` |

The absolute numbers depend on the host. Compare the ratios of one run only.
