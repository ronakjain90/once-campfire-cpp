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
| Room page | 48,309 req/s | 147,595 req/s | **3.06×** |
| Messages page | 45,430 req/s | 144,925 req/s | **3.19×** |
| Sidebar | 53,989 req/s | 156,281 req/s | **2.89×** |
| Search | 53,888 req/s | 161,070 req/s | **2.99×** |
| Post a message | 6,199 req/s | 16,073 req/s | **2.59×** |

- Over three alternating rounds, all 31.9 million C++ responses and all 14.2 million Rust responses
  were valid. The write audit verified 449,631 C++ posts and 180,230 Rust posts.
- With one writer that posts 10 messages per second, the C++ app is 2.73× to 3.13× faster on the
  four read routes. All reads were valid.
- Rust is the current port (`b7f4af0`, October 8, 2026). Both apps have a page cache for the pages
  of a signed-in user.
- The run was on an Apple M4 Mac (10 cores, 32 GB) in a Colima VM with 8 vCPUs and 16 GB. Each app
  had four pinned CPUs, and the load generator had the other four. The absolute numbers depend on
  the host. Compare the ratios.
- The [report](bench/results/2026-10-08-hit-path/report.md) gives the sources, the images, the
  setup and the change from the
  [previous run](bench/results/2026-10-08-verification/report.md).

### The page cache

The C++ app runs the SQL queries of a page, and hashes the rows that they return. If it has a page
with the same hash, it sends that page again. If not, it builds the page and keeps it. Because the
key is a hash of the data, a page cannot be stale. See section 6.1 of
[`docs/architecture.md`](docs/architecture.md).

The Rust port of October 1, 2026 had no page cache. Against that version, on the harness of this
repo ([report](bench/results/2026-10-07-page-cache-off/report.md)):

| Route (16 concurrent clients) | Rust of October 1 | C++, page cache off | C++, page cache on |
|---|---|---|---|
| Room page | 29,259 req/s | 4,376 req/s (0.15×) | 108,604 req/s (3.71×) |
| Messages page | 29,315 req/s | 5,426 req/s (0.19×) | 119,770 req/s (4.09×) |
| Sidebar | 27,100 req/s | 28,736 req/s (1.06×) | 109,097 req/s (4.03×) |
| Search | 26,579 req/s | 10,952 req/s (0.41×) | 113,849 req/s (4.28×) |
| Post a message | 6,971 req/s | 15,650 req/s (2.25×) | 15,060 req/s (2.16×) |

These numbers are older than the fragment splice (`a1a60e8`, `0375219`, `a344825`). A page build
without the page cache now joins kept gzip pieces of the page parts, as the Rust port does. It does
not compress each page again. Test runs with the page cache off, against the code before the splice:

- Room page: 9.4 times faster (3,769 to 35,357 req/s, 3 runs of `0375219`).
- Search: 6.3 times faster (8,777 to 55,133 req/s, 3 runs of `0375219`).
- Messages page: 8.2 times faster (4,448 to 36,454 req/s, 1 run of `a344825` under a profiler).

The table above is not measured again.

### Action Cable

The shared harness does not measure Action Cable. Against the current Rust port (`b7f4af0`), on
the harness of this repo ([report](bench/results/2026-10-08-cable/report.md)):

| Measurement | Rust | C++ | C++ advantage |
|---|---|---|---|
| Posts per second to all of 100 clients in one room | 2,793 | 4,809 | **1.72×** |
| Posts per second to all of 1,000 clients in one room | 496 | 534 | **1.08×** |
| Post to all 1,000 clients received, p50 | 16.3 ms | 13.7 ms | **1.19×** |
| Post to all 1,000 clients received, p99 | 20.0 ms | 17.4 ms | **1.15×** |

Every client subscribed and received every broadcast. The numbers are medians of three interleaved
runs. Under load, the C++ app keeps the broadcasts of 4 ms and sends them to each socket in one
write (`CAMPFIRE_CABLE_COALESCE_MS`, 0 to stop it).

### Image size

Against the Rust image of October 1, 2026:

| Measurement | Rust | C++ | C++ advantage |
|---|---|---|---|
| Image size, unpacked | 267 MB | 243 MB | **1.10×** |
| Image size, compressed | 70 MB | 64 MB | **1.10×** |

The pages from the two apps are almost the same size on the network (for example, 24.2 KB for a
room page from each app). Both apps join kept gzip pieces of the page parts. The decoded pages are
the same.

## Status

The open work is:

- A new measurement of the page build without the page cache, against the current Rust port.
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
