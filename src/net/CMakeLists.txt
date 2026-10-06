# HTTP server: parser, router, response, workers. T5.
add_subdirectory("${PROJECT_SOURCE_DIR}/vendor/picohttpparser" "${CMAKE_BINARY_DIR}/vendor/picohttpparser")

campfire_library(net
  SOURCES
    conn.cpp
    front.cpp
    http.cpp
    parser.cpp
    response.cpp
    router.cpp
    server.cpp
    thread_pool.cpp
    timer_wheel.cpp
    worker.cpp
    worker_conn.cpp
  PUBLIC_DEPS campfire_core campfire_picohttpparser
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
    tests/misc_test.cpp
    tests/parser_test.cpp
    tests/router_test.cpp
    tests/server_test.cpp
    "${net_test_routes_cpp}"
  WERROR)
