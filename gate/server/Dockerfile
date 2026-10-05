# syntax=docker/dockerfile:1
# Throwaway gate server (task G4). Image: campfire-gate:app
#   docker build -t campfire-gate:app gate/server
FROM debian:trixie AS build
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends clang lld cmake ninja-build libssl-dev libdeflate-dev \
      libjemalloc-dev libcrypt-dev ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt .
COPY vendor vendor
COPY src src
RUN CC=clang CXX=clang++ cmake -S . -B build -G Ninja && cmake --build build

FROM debian:trixie-slim
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends libssl3t64 libdeflate0 libjemalloc2 libcrypt1 libstdc++6 \
      && rm -rf /var/lib/apt/lists/* && mkdir -p /rails/storage/db /rails/storage/files && chown -R 1000:1000 /rails
COPY --from=build /src/build/gate-server /app/gate-server
COPY --from=build /src/build/cookie_test /app/cookie_test
COPY fixtures /app/fixtures
WORKDIR /rails
USER 1000:1000
ENV RAILS_ENV=production GATE_FIXTURES=/app/fixtures
EXPOSE 80
CMD ["/app/gate-server"]
