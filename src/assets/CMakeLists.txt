# Assets (architecture section 15): Propshaft digests, import map and precompressed bodies.
#   campfire_assets_base   mime types (runtime and generator)
#   campfire_assets_build  Propshaft and import map code (build time only)
#   campfire_assets_gen    the generator: writes assets_blob.bin and assets_data.cpp
#   campfire_assets        the runtime library with the generated data
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PCRE2 REQUIRED IMPORTED_TARGET libpcre2-8)
pkg_check_modules(LIBDEFLATE REQUIRED IMPORTED_TARGET libdeflate)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)

campfire_library(assets_base SOURCES mime.cpp WERROR)

campfire_library(assets_build
  SOURCES importmap.cpp propshaft.cpp regex.cpp
  PUBLIC_DEPS campfire_core PkgConfig::PCRE2 OpenSSL::Crypto campfire_assets_base
  WERROR)

campfire_executable(campfire_assets_gen
  SOURCES gen/main.cpp
  DEPS campfire_assets_build campfire_compat PkgConfig::LIBDEFLATE PkgConfig::ZSTD
  WERROR)
target_include_directories(campfire_assets_gen PRIVATE "${PROJECT_SOURCE_DIR}/src")

set(CAMPFIRE_RAILS_ASSETS_DIR "${PROJECT_SOURCE_DIR}/vendor/rails-assets")
set(CAMPFIRE_ASSETS_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(GLOB_RECURSE campfire_asset_inputs CONFIGURE_DEPENDS "${CAMPFIRE_RAILS_ASSETS_DIR}/*")
add_custom_command(
  OUTPUT "${CAMPFIRE_ASSETS_GEN_DIR}/assets_data.cpp" "${CAMPFIRE_ASSETS_GEN_DIR}/assets_blob.bin"
  COMMAND campfire_assets_gen "${CAMPFIRE_RAILS_ASSETS_DIR}" "${CAMPFIRE_ASSETS_GEN_DIR}"
  DEPENDS campfire_assets_gen ${campfire_asset_inputs}
  COMMENT "Generating the asset table"
  VERBATIM)
set_source_files_properties("${CAMPFIRE_ASSETS_GEN_DIR}/assets_data.cpp" PROPERTIES
  OBJECT_DEPENDS "${CAMPFIRE_ASSETS_GEN_DIR}/assets_blob.bin")

campfire_library(assets
  SOURCES assets.cpp static_files.cpp "${CAMPFIRE_ASSETS_GEN_DIR}/assets_data.cpp"
  PUBLIC_DEPS campfire_core campfire_compat campfire_assets_base
  WERROR)

campfire_test(assets
  SOURCES tests/propshaft_test.cpp tests/reference_test.cpp
  DEPS campfire_assets_build
  WERROR)
target_compile_definitions(test_assets PRIVATE
  CAMPFIRE_ASSETS_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
  CAMPFIRE_RAILS_ASSETS="${CAMPFIRE_RAILS_ASSETS_DIR}")
