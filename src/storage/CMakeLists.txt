# Storage: Active Storage, libvips, ffmpeg (architecture section 12). Library: campfire_storage.
find_package(PkgConfig REQUIRED)
pkg_check_modules(VIPS REQUIRED IMPORTED_TARGET vips)

campfire_library(storage
  SOURCES
    analyze.cpp
    blob.cpp
    disk.cpp
    disposition.cpp
    filename.cpp
    key.cpp
    marcel.cpp
    marcel_tables.cpp
    media_pool.cpp
    paths.cpp
    process.cpp
    storage.cpp
    transform.cpp
    vips.cpp
  PUBLIC_DEPS campfire_core campfire_compat PkgConfig::VIPS
  WERROR)
