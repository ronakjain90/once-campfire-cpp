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
