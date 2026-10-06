# syntax = docker/dockerfile:1
#
# Production image for the C++ port. A drop-in for campfire-rust:app: same user (uid 1000), working
# directory, storage layout (/rails/storage/{db,files,backups}), environment, ports and hooks.
#
#   docker build -t campfire-cpp:app -f docker/Dockerfile .
#
# The media stages (media-base, vips, ffmpeg, media) are copied from the Rust Dockerfile: same
# sources, versions, checksums, configure options and omitted loaders and formats, so thumbnails
# are byte-identical. Only the base image differs: debian:trixie plus build-essential, in place of
# rust:trixie (same Debian release and the same gcc).
#
# The cpp-build stage runs `cmake --preset release` (binary /build/release/campfire).

ARG DEBIAN_RELEASE=trixie
ARG LIBVIPS_VERSION=8.16.1-1+deb13u1
ARG LIBVIPS_DSC_SHA256=60205e00d061b9d8072938e04899f2ca2fdac0513068e561d33f7c87fae1ae2e
ARG FFMPEG_VERSION=7:7.1.5-0+deb13u1
ARG FFMPEG_DSC_SHA256=9ed2ed34cbe7f056eeebbe9045c5e2d15e41b5b053fe7c8ba6979a0b6fb081ce


# Toolchain and Debian -dev packages for libvips and ffmpeg, with deb-src enabled so `apt-get source`
# can fetch the exact Debian sources (the .dsc checksums pin them).
FROM docker.io/library/debian:${DEBIAN_RELEASE} AS media-base
RUN sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/debian.sources && \
    apt-get update -qq && \
    apt-get install --no-install-recommends -y \
      build-essential ca-certificates pkg-config \
      dpkg-dev meson ninja-build nasm \
      libglib2.0-dev libexpat1-dev zlib1g-dev libjpeg62-turbo-dev libspng-dev libpng-dev \
      libwebp-dev libtiff-dev libheif-dev libexif-dev liblcms2-dev libcgif-dev libimagequant-dev \
      libhwy-dev libdav1d-dev libbz2-dev liblzma-dev && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /usr/src


# libvips with only the loaders and savers the Rust image has, built into the library (no modules),
# with Debian's build flags. Installs headers, libvips.so and vips.pc under /opt/vips.
FROM media-base AS vips
ARG LIBVIPS_VERSION
ARG LIBVIPS_DSC_SHA256
RUN apt-get update -qq && \
    apt-get source -qq vips=${LIBVIPS_VERSION} && \
    echo "${LIBVIPS_DSC_SHA256}  vips_${LIBVIPS_VERSION}.dsc" | sha256sum -c - && \
    cd vips-${LIBVIPS_VERSION%-*} && \
    eval "$(DEB_BUILD_MAINT_OPTIONS=hardening=+all dpkg-buildflags --export=sh)" && \
    meson setup build --buildtype=plain --wrap-mode=nodownload --prefix=/opt/vips --libdir=lib \
      --auto-features=disabled -Dmodules=disabled -Dintrospection=disabled -Dcplusplus=false \
      -Ddeprecated=false -Dexamples=false \
      -Djpeg=enabled -Dspng=enabled -Dpng=enabled -Dwebp=enabled -Dtiff=enabled -Dheif=enabled \
      -Dexif=enabled -Dlcms=enabled -Dcgif=enabled -Dimagequant=enabled -Dhighway=enabled \
      -Dzlib=enabled && \
    meson install -C build --strip


# ffmpeg and ffprobe (shared libraries) from Debian's source, with Debian's toolchain and version
# string. Installs headers, libraries and .pc files under /opt/ffmpeg.
FROM media-base AS ffmpeg
ARG FFMPEG_VERSION
ARG FFMPEG_DSC_SHA256
RUN apt-get update -qq && \
    apt-get source -qq ffmpeg=${FFMPEG_VERSION} && \
    upstream=${FFMPEG_VERSION#*:} && upstream=${upstream%-*} && revision=${FFMPEG_VERSION##*-} && \
    echo "${FFMPEG_DSC_SHA256}  ffmpeg_${FFMPEG_VERSION#*:}.dsc" | sha256sum -c - && \
    cd ffmpeg-${upstream} && \
    ./configure --prefix=/opt/ffmpeg --extra-version="${revision}" --toolchain=hardened \
      --enable-shared --disable-static --disable-doc --disable-ffplay --disable-avdevice \
      --disable-autodetect --disable-network --disable-hwaccels --disable-devices \
      --enable-libdav1d --enable-zlib --enable-bzlib --enable-lzma \
      --disable-encoders --enable-encoder=mjpeg \
      --disable-muxers --enable-muxer=image2 \
      --disable-protocols --enable-protocol=file,pipe \
      --disable-filters \
      --enable-filter=buffer,buffersink,abuffer,abuffersink,format,aformat,null,anull,scale,aresample \
      --enable-filter=select,loop,trim,transpose,hflip,vflip,rotate,crop && \
    make -j"$(nproc)" && \
    make install && \
    strip --strip-unneeded /opt/ffmpeg/lib/*.so.* /opt/ffmpeg/bin/*


# The shared libraries and executables that go into the runtime, in one directory tree.
FROM scratch AS media
COPY --from=vips /opt/vips/lib/libvips.so.42 /usr/local/lib/
COPY --from=ffmpeg /opt/ffmpeg/lib/libavcodec.so.61 /opt/ffmpeg/lib/libavfilter.so.10 \
     /opt/ffmpeg/lib/libavformat.so.61 /opt/ffmpeg/lib/libavutil.so.59 \
     /opt/ffmpeg/lib/libswresample.so.5 /opt/ffmpeg/lib/libswscale.so.8 /usr/local/lib/
COPY --from=ffmpeg /opt/ffmpeg/bin/ffmpeg /opt/ffmpeg/bin/ffprobe /usr/local/bin/


# C++ build: clang 19, CMake, Ninja and the libraries of docker/dev.Dockerfile, with the libvips and
# ffmpeg of the stages above in place of Debian's (headers, libraries and .pc files).
FROM media-base AS cpp-build
RUN apt-get update -qq && \
    apt-get install --no-install-recommends -y \
      clang-19 lld-19 llvm-19 libclang-rt-19-dev \
      cmake ninja-build pkg-config python3 python3-yaml \
      libssl-dev libdeflate-dev libjemalloc-dev libzstd-dev zlib1g-dev libnghttp2-dev libpcre2-dev && \
    rm -rf /var/lib/apt/lists/* && \
    for t in clang clang++ lld ld.lld llvm-ar llvm-ranlib; do ln -sf /usr/bin/$t-19 /usr/local/bin/$t; done
COPY --from=vips /opt/vips /opt/vips
COPY --from=ffmpeg /opt/ffmpeg /opt/ffmpeg
ENV CC=clang CXX=clang++ \
    PKG_CONFIG_PATH=/opt/vips/lib/pkgconfig:/opt/ffmpeg/lib/pkgconfig \
    LIBRARY_PATH=/opt/vips/lib:/opt/ffmpeg/lib \
    LD_LIBRARY_PATH=/opt/vips/lib:/opt/ffmpeg/lib
# Fail early if the stage-built libraries are not the ones that pkg-config finds.
RUN test "$(pkg-config --modversion vips)" = "8.16.1" && \
    test "$(pkg-config --modversion libavcodec)" = "61.19.101" || \
    { pkg-config --modversion vips libavcodec; exit 1; }

WORKDIR /src
COPY . .
RUN mkdir -p /out && \
    cmake --preset release && cmake --build --preset release --target campfire && \
    install -D -m 755 /build/release/campfire /out/campfire


FROM docker.io/library/debian:${DEBIAN_RELEASE}-slim

# The Debian libraries libvips and ffmpeg were built against (same list as the Rust image).
RUN apt-get update -qq && \
    apt-get install --no-install-recommends -y \
      ca-certificates libjemalloc2 libglib2.0-0t64 libexpat1 libjpeg62-turbo libspng0 libpng16-16t64 \
      libwebp7 libwebpmux3 libwebpdemux2 libtiff6 libheif1 libheif-plugin-libde265 \
      libheif-plugin-dav1d libexif12 liblcms2-2 libcgif0 libimagequant0 libhwy1t64 libdav1d7 && \
    rm -rf /var/lib/apt/lists /var/cache/apt/archives

COPY --from=media / /
RUN ldconfig

# Image metadata
ARG OCI_DESCRIPTION
LABEL org.opencontainers.image.description="${OCI_DESCRIPTION}"
ARG OCI_SOURCE
LABEL org.opencontainers.image.source="${OCI_SOURCE}"
LABEL org.opencontainers.image.licenses="MIT"

# Run and own only the runtime files as a non-root user, as the reference does.
RUN groupadd --system --gid 1000 rails && \
    useradd rails --uid 1000 --gid 1000 --create-home --shell /bin/bash

WORKDIR /rails

COPY --from=cpp-build /out/campfire /usr/local/bin/campfire

# bin/boot: starts the server.
COPY --chmod=755 <<'EOF2' /rails/bin/boot
#!/bin/sh
exec /usr/local/bin/campfire server
EOF2

# The storage root is Rails.root.join("storage"): storage/db, storage/files, storage/backups.
RUN mkdir -p /rails/storage/db /rails/storage/files /rails/storage/backups && \
    chown -R 1000:1000 /rails

# ONCE backup/restore hooks. pre-backup calls `campfire backup` (not built yet).
# post-restore is the reference's script (docker/hooks/post-restore).
COPY --chmod=755 <<'EOF2' /hooks/pre-backup
#!/bin/bash
cd /rails
exec /usr/local/bin/campfire backup
EOF2
COPY --chmod=755 docker/hooks/post-restore /hooks/post-restore

USER 1000:1000

ENV RAILS_ENV="production"
ENV HTTP_IDLE_TIMEOUT=60
ENV HTTP_READ_TIMEOUT=300
ENV HTTP_WRITE_TIMEOUT=300

ARG APP_VERSION
ENV APP_VERSION=$APP_VERSION
ARG GIT_REVISION
ENV GIT_REVISION=$GIT_REVISION

EXPOSE 80 443

CMD ["bin/boot"]
