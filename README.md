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
[room page, sidebar and post](bench/results/2026-10-07-b0-rep4/report.md),
[messages page and search](bench/results/2026-10-07-messages-search/report.md) and
[Action Cable](bench/results/2026-10-07-cable/report.md).

### HTTP throughput (16 concurrent clients)

| Route | Rust | C++ | C++ advantage |
|---|---|---|---|
| Room page | 25,779 req/s | 37,746 req/s | **1.46×** |
| Messages page | 29,966 req/s | 61,390 req/s | **2.05×** |
| Sidebar | 24,044 req/s | 67,072 req/s | **2.79×** |
| Search | 25,547 req/s | 46,075 req/s | **1.80×** |
| Post a message | 7,068 req/s | 11,699 req/s | **1.66×** |

At 64 clients, the advantage is 1.51× for the room page, 2.02× for the messages page, 3.12× for
the sidebar, 1.34× for search and 2.10× for a post. The C++ app also uses less CPU: 0.085 ms for
each room page, against 0.13 ms.

### Latency and real time

| Measurement | Rust | C++ | C++ advantage |
|---|---|---|---|
| Room page p99, 64 clients | 4.5 ms | 2.9 ms | **1.56×** |
| Post a message p99, 64 clients | 14.3 ms | 8.1 ms | **1.76×** |
| Posts per second to all of 100 clients in one room | 3,098 | 3,592 | **1.16×** |
| Posts per second to all of 1,000 clients in one room | 546 | 479 | 0.88× |
| Post to all 1,000 clients received, p50 | 15.9 ms | 14.1 ms | **1.13×** |
| Post to all 1,000 clients received, p99 | 20.3 ms | 18.2 ms | **1.12×** |

Every client subscribed and received every broadcast. The Action Cable numbers are medians of three
interleaved runs. The HTTP numbers come from one run for each app.

### Image size

| Measurement | Rust | C++ | C++ advantage |
|---|---|---|---|
| Image size, unpacked | 267 MB | 243 MB | **1.10×** |
| Image size, compressed | 70 MB | 64 MB | **1.10×** |

Pages from the C++ app are smaller on the network (for example, 20.7 KB against 24.2 KB for a room
page). The cause is the compressor: the C++ app uses libdeflate. The decoded pages are the same.

## Status

The goal is 1.5 times the throughput of the Rust port on each route. The open work is:

- The room page at 16 clients, search at 64 clients, and the Action Cable fan-out with 1,000
  clients.
- Parity checks against Rails: the Playwright harness, and database and cookie compatibility in
  both directions.
- A full benchmark of all routes with three interleaved runs.

The plan is in [`plans/cpp-port.md`](plans/cpp-port.md). The status of each task is in
[`plans/tasks.md`](plans/tasks.md).

## Development

Run all commands through `bin/dev`. It runs them in the `campfire-cpp-dev` Docker image, with
clang 19, CMake and Ninja.

```sh
bin/dev build release
bin/dev test release
bin/dev test asan
bin/dev test tsan
bin/dev format --check
tools/diffsweep/diffsweep --expected campfire-rust:app --actual campfire-cpp
gate/bench/run --apps rust=campfire-rust:app,cpp=campfire-cpp --routes room_show,sidebar,post_message
```

- The presets are `release`, `asan` (ASan and UBSan) and `tsan`. All warnings are errors.
- The diff sweep starts both images on the same seed and compares each response byte for byte.
  See [`tools/diffsweep/README.md`](tools/diffsweep/README.md).
- The benchmark takes `--cable "100 1000"` for the Action Cable fan-out. See
  [`gate/bench/README.md`](gate/bench/README.md). Stop all other work on the host before a run.
- See [`AGENTS.md`](AGENTS.md) for the repository layout and the working rules, and
  [`plans/security-review.md`](plans/security-review.md) for the security review.

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

[`plans/divergences.md`](plans/divergences.md) lists each difference and how the diff sweep
accepts it.

</details>

## License

MIT. See [`MIT-LICENSE`](MIT-LICENSE).
