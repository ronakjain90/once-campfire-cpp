# HTTP server: parser, router, response, workers. T5.
add_subdirectory("${PROJECT_SOURCE_DIR}/vendor/picohttpparser" "${CMAKE_BINARY_DIR}/vendor/picohttpparser")

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(NGHTTP2 REQUIRED IMPORTED_TARGET libnghttp2)
pkg_check_modules(LIBDEFLATE REQUIRED IMPORTED_TARGET libdeflate)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)

campfire_library(net
  SOURCES
    conn.cpp
    front.cpp
    front/acme.cpp
    front/acme_account.cpp
    front/acme_flow.cpp
    front/acme_http.cpp
    front/cache.cpp
    front/compress.cpp
    front/front.cpp
    front/headers.cpp
    front/static_files.cpp
    front/tls.cpp
    front/tls_keys.cpp
    http.cpp
    parser.cpp
    response.cpp
    router.cpp
    server.cpp
    thread_pool.cpp
    timer_wheel.cpp
    worker.cpp
    worker_conn.cpp
    worker_h2.cpp
    worker_io.cpp
    worker_ws.cpp
  PUBLIC_DEPS campfire_core campfire_picohttpparser campfire_assets OpenSSL::SSL OpenSSL::Crypto
  DEPS campfire_compat PkgConfig::LIBDEFLATE PkgConfig::ZSTD PkgConfig::NGHTTP2
  WERROR)

# A route table for the router tests.
file(GLOB net_test_routes CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.inc")
set(net_test_routes_cpp "${CMAKE_CURRENT_BINARY_DIR}/test_routes.gen.cpp")
add_custom_command(
  OUTPUT "${net_test_routes_cpp}"
  COMMAND python3 "${PROJECT_SOURCE_DIR}/tools/routes_gen.py" --out "${net_test_routes_cpp}"
          --namespace campfire::app_test ${net_test_routes}
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/routes_gen.py" ${net_test_routes})

campfire_test(net
  SOURCES
    tests/acme_test.cpp
    tests/front_test.cpp
    tests/h2_test.cpp
    tests/misc_test.cpp
    tests/parser_test.cpp
    tests/router_test.cpp
    tests/server_test.cpp
    tests/ws_test.cpp
    "${net_test_routes_cpp}"
  WERROR)

# libFuzzer target of the request parser: cmake -DCAMPFIRE_FUZZ=ON (use the asan preset).
option(CAMPFIRE_FUZZ "Build the fuzz targets" OFF)
if(CAMPFIRE_FUZZ)
  add_executable(fuzz_http_parser fuzz/parser_fuzz.cpp)
  target_link_libraries(fuzz_http_parser PRIVATE campfire_net campfire_options)
  target_compile_options(fuzz_http_parser PRIVATE -fsanitize=fuzzer)
  target_link_options(fuzz_http_parser PRIVATE -fsanitize=fuzzer)
endif()
