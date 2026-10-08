# Campfire in C++

A C++ implementation of [ONCE Campfire](https://github.com/basecamp/once-campfire), and a drop-in
replacement for the [Rust port](https://github.com/basecamp/once-campfire-rust). It uses the same
SQLite database, storage layout and signed and encrypted cookies. Existing installs upgrade with no
data migration, and no user has to sign in again.

One `campfire` executable serves the whole app, with libvips and ffmpeg for media. It includes TLS
with automatic certificates, HTTP/2, Web Push, bot webhooks, search and Action Cable-compatible
WebSockets. For each of the 689 requests of the diff sweep, the C++ app and the Rust app send the
same response.

## Running it

Build the image:

```sh
docker build -t campfire-cpp -f docker/Dockerfile .
```

The image has the same user, ports, storage layout, entrypoint and environment variables as the
Rust image. Run it as you run the Rust image:

```sh
docker run -d -p 80:80 -p 443:443 \
  -e SECRET_KEY_BASE=... -e VAPID_PUBLIC_KEY=... -e VAPID_PRIVATE_KEY=... \
  -e TLS_DOMAIN=chat.example.com \
  -v campfire:/rails/storage \
  campfire-cpp
```

- `TLS_DOMAIN` enables automatic Let's Encrypt certificates. `DISABLE_SSL` enables plain HTTP.
- `/rails/storage` holds the database, uploads, backups and certificates. To move an install from
  the Rust image, keep its storage volume and its secrets.
- The other settings are the settings of the Rust port. The C++ app reads them in
  [`src/core/config.cpp`](src/core/config.cpp).

## Performance

### Shared verification harness

These numbers come from the shared
[verification harness](https://github.com/basecamp/once-campfire-verification) of the Campfire
ports. It checks every measured response against its route contract: status, headers, the complete
decoded body and the expected messages. It also checks every acknowledged post against its row,
body, room and search-index entry. Any failure stops the run.

| Route (16 concurrent clients) | Rust | C++ | C++ advantage |
|---|---|---|---|
| Room page | 47,967 req/s | 102,650 req/s | **2.14×** |
| Messages page | 44,983 req/s | 117,679 req/s | **2.62×** |
| Sidebar | 52,494 req/s | 107,246 req/s | **2.04×** |
| Search | 53,167 req/s | 116,777 req/s | **2.20×** |
| Post a message | 6,075 req/s | 13,718 req/s | **2.26×** |

- Over three alternating rounds, all 28.0 million C++ responses and all 13.8 million Rust responses
  were valid. The write audit verified 386,877 C++ posts and 178,582 Rust posts.
- Rust is the current port (`b7f4af0`, October 8, 2026). Both apps have a page cache for the pages
  of a signed-in user.
- The run was on an Apple M4 Mac (10 cores, 32 GB) in a Colima VM with 8 vCPUs and 16 GB. Each app
  had four pinned CPUs, and the load generator had the other four. The absolute numbers depend on
  the host. Compare the ratios.
- The harness does not list the C++ app. A two-line local change adds it. The
  [report](bench/results/2026-10-08-verification/report.md) gives the change, the sources, the
  images and the setup.

### The page cache

The C++ app runs the SQL queries of a page, and hashes the rows that they return. If it has a page
with the same hash, it sends that page again. If not, it builds the page and keeps it. Because the
key is a hash of the data, a page cannot be stale. See section 6.1 of
[`docs/architecture.md`](docs/architecture.md).

Without the page cache (`CAMPFIRE_PAGE_CACHE_MB=0`), the C++ app builds the room, messages and
search pages slowly. The Rust port of October 1, 2026 had no page cache. Against that version, on
the harness of this repo
([report](bench/results/2026-10-07-page-cache-off/report.md)):

| Route (16 concurrent clients) | Rust of October 1 | C++, page cache off | C++, page cache on |
|---|---|---|---|
| Room page | 29,259 req/s | 4,376 req/s (0.15×) | 108,604 req/s (3.71×) |
| Messages page | 29,315 req/s | 5,426 req/s (0.19×) | 119,770 req/s (4.09×) |
| Sidebar | 27,100 req/s | 28,736 req/s (1.06×) | 109,097 req/s (4.03×) |
| Search | 26,579 req/s | 10,952 req/s (0.41×) | 113,849 req/s (4.28×) |
| Post a message | 6,971 req/s | 15,650 req/s (2.25×) | 15,060 req/s (2.16×) |

### Action Cable

Against the Rust port of October 1, 2026, on the harness of this repo
([report](bench/results/2026-10-07-cable/report.md)):

| Measurement | Rust | C++ | C++ advantage |
|---|---|---|---|
| Posts per second to all of 100 clients in one room | 3,098 | 3,592 | **1.16×** |
| Posts per second to all of 1,000 clients in one room | 546 | 479 | 0.88× |
| Post to all 1,000 clients received, p50 | 15.9 ms | 14.1 ms | **1.13×** |
| Post to all 1,000 clients received, p99 | 20.3 ms | 18.2 ms | **1.12×** |

Every client subscribed and received every broadcast. The numbers are medians of three interleaved
runs.

### Image size

Against the Rust image of October 1, 2026:

| Measurement | Rust | C++ | C++ advantage |
|---|---|---|---|
| Image size, unpacked | 267 MB | 243 MB | **1.10×** |
| Image size, compressed | 70 MB | 64 MB | **1.10×** |

Pages from the C++ app are smaller on the network (for example, 20.7 KB against 24.2 KB for a room
page). The cause is the compressor: the C++ app uses libdeflate. The decoded pages are the same.

## Status

The open work is:

- The page build without the page cache. It is slower than the Rust port of October 1 for the room,
  messages and search pages.
- The Action Cable fan-out with 1,000 clients. It is 0.88 times the Rust port of October 1.
- The browser checks of the shared verification harness (`bin/browser`).
- Parity checks against Rails: the Playwright harness, and database and cookie compatibility in
  both directions.

## Development

Run all commands through `bin/dev`. It runs them in the `campfire-cpp-dev` Docker image, with
clang 19, CMake and Ninja. The diff sweep and the benchmark also need a clone of the
[Rust port](https://github.com/basecamp/once-campfire-rust) in `../once-campfire-rust`. To use a
different parent folder, set `CAMPFIRE_WORKSPACE`.

```sh
bin/dev build release
bin/dev test release
bin/dev test asan
bin/dev test tsan
bin/dev format --check
tools/diffsweep/diffsweep --expected campfire-rust:app --actual campfire-cpp
bench/run --apps rust=campfire-rust:app,cpp=campfire-cpp --reps 3
```

- The presets are `release`, `asan` (ASan and UBSan) and `tsan`. All warnings are errors.
- The diff sweep starts both images on the same seed and compares each response byte for byte.
  See [`tools/diffsweep/README.md`](tools/diffsweep/README.md).
- The benchmark compares the C++ image with the Rust image on the same host. It needs a Docker host
  with 8 CPUs, the seed data of the Rust port and the two images. Do the setup in
  [`bench/README.md`](bench/README.md) one time. The script checks the setup before it starts.
  Stop all other work on the host before a run.
- See [`AGENTS.md`](AGENTS.md) for the build rules, [`docs/architecture.md`](docs/architecture.md)
  for the design and [`docs/security-review.md`](docs/security-review.md) for the security review.

| Folder | Contents |
|---|---|
| `src/` | The app, one library for each folder |
| `tests/` | Shared test code, golden vectors and fuzz seeds. The unit tests are in `src/*/tests/`. |
| `tools/` | Code generators, the diff sweep and the fuzz scripts |
| `bench/` | The benchmark harness and the reports |
| `vendor/` | SQLite, picohttpparser, xxHash, crypt_blowfish, doctest and the frontend assets |
| `docker/` | The production image and the development image |
| `docs/` | The design, the known differences and the security review |

## Known differences

The C++ app copies the deliberate differences of the Rust port from Rails. See the
[Known differences](https://github.com/basecamp/once-campfire-rust#known-differences) of the Rust
port.

<details>
<summary>Differences from the Rust port</summary>

- **Compression:** the C++ app compresses with libdeflate and zstd. Thus the compressed bytes and
  `content-length` are different from the Rust app. The decoded bodies are the same.
- **Static files:** `last-modified` is the time when the image was built, as in the Rust app. Thus
  the value is different for each build.

[`docs/divergences.md`](docs/divergences.md) lists each difference and how the diff sweep
accepts it.

</details>

## License

MIT. See [`MIT-LICENSE`](MIT-LICENSE).
