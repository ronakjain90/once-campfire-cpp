# Security and cache checks, 2026-10-09

`bench/controls` runs the checks of the production controls in `docs/performance-review.md` of the
[shared verification harness](https://github.com/basecamp/once-campfire-verification). The harness
does not publish those controls, so this repo has its own. Each check runs on a fresh copy of the
harness seed (`4d1da88`), with the page cache on (32 MB) and off (0).

## Result

| Image | Source | Checks |
|---|---|---|
| C++ `sha256:bb7600c3…` | `00e2349`, clean (measured as `decb7e1`, the same tree before a rebase) | **90 of 90 pass** ([cpp/report.md](cpp/report.md)) |
| Rust `sha256:b1a974fe…` | `b7f4af0` | 88 of 88 pass ([rust/report.md](rust/report.md)) |

The Rust image calibrates the checks: it passed the controls of the shared review, and it passes
every check here. The C++ run has two more checks: a session made by one image works in the other
image on the same storage, in both directions.

## What the checks cover

- **Fetch Metadata:** same-origin and same-site posts work, and a post with no metadata over plain
  HTTP works. Cross-site, `none`, invalid values, `Origin: null` and a foreign Origin get 422, and
  store no message. A token from an old tab is ignored: it does not break a same-origin post, and it
  does not allow a cross-site post.
- **Signed capabilities:** a signed blob link, a changed blob link, a direct upload with no session,
  a forged session-transfer token, a wrong join code, a changed avatar token, and valid and wrong
  bot keys.
- **gzip negotiation:** `gzip`, `gzip;q=0`, `gzip;q=0.000`, `identity`, `*;q=0, identity`, and
  `gzip;q=0` after 17 other codings. The decoded body is always the same.
- **Direct SQL edits:** another process changes a message body, a creator or a boost with no
  timestamp change. The room page, the permalink and search show the change at once.
- **Conditional requests:** after a direct SQL edit, an old ETag and a future `If-Modified-Since`
  get the new page. The current ETag gets 304.
- **Revocation:** a deleted session, a deleted membership (room page, messages page, search, post),
  a deactivated user (page and sign-in) and a changed bot key.

## Bugs that the checks found

The first C++ run passed 60 of 86 checks. Some failures were mistakes in the script; the Rust
calibration found them. The others were bugs in the C++ app, and this branch fixes them:

| Bug | Fix |
|---|---|
| Message and boost fragments were keyed by id and `updated_at`. After a direct SQL edit, the pages showed the old body, creator and boosts, and an old ETag got 304. | `0e4969c`: a commit epoch from `PRAGMA data_version` is in the fragment keys, the page keys and the ETags. |
| The session cache of a worker forgot an entry only for a change of the app's own writer. A session that SQL deleted stayed signed in. | `0e4969c`: a new epoch drops the session cache. |
| A job thread, or a request after a wait, could take a newer epoch after its rows (the flaw of once-campfire-elixir #7). | `00e2349`: the fragment cache works only in a read transaction. |

## Run

```sh
bench/controls --image campfire-cpp --rust-image campfire-rust:app
```

The script needs the harness checkout in `../once-campfire-verification` (after `bin/seed`), the
dev image (for `sqlite3`) and port 3190.
