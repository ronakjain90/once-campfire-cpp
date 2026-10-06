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

# Golden vectors: spec/vectors/. Fixtures: read in place from the Rust repo (51 MB, not copied).
set(CAMPFIRE_VECTORS_DIR "${PROJECT_SOURCE_DIR}/spec/vectors" CACHE PATH "Directory of the golden vector JSON files")
set(CAMPFIRE_FIXTURES_DIR "${PROJECT_SOURCE_DIR}/../../once-campfire-rust/reference/test/fixtures/files" CACHE PATH "Directory of the media fixture files")

campfire_test(storage
  SOURCES
    tests/test_vectors.cpp
    tests/test_pipeline.cpp
  WERROR)
target_compile_definitions(test_storage PRIVATE
  CAMPFIRE_VECTORS_DIR="${CAMPFIRE_VECTORS_DIR}" CAMPFIRE_FIXTURES_DIR="${CAMPFIRE_FIXTURES_DIR}")
set_tests_properties(test_storage PROPERTIES
  ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/tsan.supp")
