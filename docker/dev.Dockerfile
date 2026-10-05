# Development image: the toolchain and libraries for building and testing the app.
# The production image (docker/Dockerfile) builds libvips and ffmpeg at the exact versions.
FROM docker.io/library/debian:trixie
RUN apt-get update && apt-get install -y --no-install-recommends \
      clang-19 lld-19 llvm-19 libclang-rt-19-dev clang-format-19 clang-tidy-19 \
      cmake ninja-build pkg-config git ca-certificates curl python3 python3-yaml sqlite3 \
      libssl-dev libdeflate-dev libjemalloc-dev libzstd-dev zlib1g-dev libnghttp2-dev libpcre2-dev \
      libvips-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
      gdb procps \
    && rm -rf /var/lib/apt/lists/* \
    && for t in clang clang++ lld ld.lld llvm-ar llvm-ranlib llvm-symbolizer clang-format clang-tidy; do \
         ln -sf /usr/bin/$t-19 /usr/local/bin/$t; done
ENV CC=clang CXX=clang++
