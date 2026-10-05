# Runner image for the gate benchmark harness (campfire-bench-runner).
# Build context: once-campfire-rust/bench/loadgen (the harness script `run` does this):
#   docker build -t campfire-bench-runner -f gate/bench/Dockerfile ../once-campfire-rust/bench/loadgen
FROM docker.io/library/rust:1.98.1-trixie AS loadgen
WORKDIR /src
COPY Cargo.toml Cargo.lock ./
COPY src ./src
RUN cargo build --release --locked

FROM docker.io/library/docker:cli AS dockercli

FROM docker.io/library/debian:trixie-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends python3 util-linux procps curl sqlite3 ca-certificates \
 && rm -rf /var/lib/apt/lists/*
COPY --from=dockercli /usr/local/bin/docker /usr/local/bin/docker
COPY --from=loadgen /src/target/release/loadgen /usr/local/bin/loadgen
