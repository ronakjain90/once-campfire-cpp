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

Production images, the same seed data and four pinned CPUs for each app, on an Apple M4 Mac
(10 cores, 32 GB). Docker runs in a Colima VM with 8 vCPUs and 16 GB. The load generator of the
Rust repo runs on the other four CPUs. All runs were on October 7, 2026, on a quiet host. The
reports record the settings and the ranges:
[HTTP](bench/results/2026-10-07-read-tx-rust/report.md) and
[Action Cable](bench/results/2026-10-07-cable/report.md).

### HTTP throughput (16 concurrent clients)

| Route | Rust | C++ | C++ advantage |
|---|---|---|---|
| Room page | 27,754 req/s | 108,112 req/s | **3.90×** |
| Messages page | 28,091 req/s | 113,318 req/s | **4.03×** |
| Sidebar | 28,199 req/s | 108,719 req/s | **3.86×** |
| Search | 26,288 req/s | 118,928 req/s | **4.52×** |
| Post a message | 7,314 req/s | 15,240 req/s | **2.08×** |

### HTTP throughput (100 concurrent clients)

| Route | Rust | C++ | C++ advantage |
|---|---|---|---|
| Room page | 26,989 req/s | 132,240 req/s | **4.90×** |
| Messages page | 30,805 req/s | 135,390 req/s | **4.40×** |
| Sidebar | 28,709 req/s | 124,271 req/s | **4.33×** |
| Search | 34,172 req/s | 144,563 req/s | **4.23×** |
| Post a message | 7,320 req/s | 21,097 req/s | **2.88×** |

The C++ app also uses less CPU: 0.030 ms for each room page, against 0.13 ms.

### Latency and real time

| Measurement | Rust | C++ | C++ advantage |
|---|---|---|---|
| Room page p99, 100 clients | 7.4 ms | 1.8 ms | **4.15×** |
| Post a message p99, 100 clients | 20.7 ms | 11.8 ms | **1.75×** |
| Posts per second to all of 100 clients in one room | 3,098 | 3,592 | **1.16×** |
| Posts per second to all of 1,000 clients in one room | 546 | 479 | 0.88× |
| Post to all 1,000 clients received, p50 | 15.9 ms | 14.1 ms | **1.13×** |
| Post to all 1,000 clients received, p99 | 20.3 ms | 18.2 ms | **1.12×** |

Every client subscribed and received every broadcast. All numbers are medians of three interleaved
runs. The HTTP report also has the results for 64 clients.

### Image size

| Measurement | Rust | C++ | C++ advantage |
|---|---|---|---|
| Image size, unpacked | 267 MB | 243 MB | **1.10×** |
| Image size, compressed | 70 MB | 64 MB | **1.10×** |

Pages from the C++ app are smaller on the network (for example, 20.7 KB against 24.2 KB for a room
page). The cause is the compressor: the C++ app uses libdeflate. The decoded pages are the same.

## Status

The open work is:

- The Action Cable fan-out with 1,000 clients. It is 0.88 times the Rust port.
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
