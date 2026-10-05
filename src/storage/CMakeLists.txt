# Storage: Active Storage, libvips, ffmpeg (architecture section 12). Library: campfire_storage.
find_package(PkgConfig REQUIRED)
pkg_check_modules(VIPS REQUIRED IMPORTED_TARGET vips)

campfire_library(storage
  SOURCES
    blob.cpp
    disposition.cpp
    filename.cpp
    key.cpp
    marcel.cpp
    marcel_tables.cpp
    process.cpp
  PUBLIC_DEPS campfire_core campfire_compat PkgConfig::VIPS
  WERROR)
