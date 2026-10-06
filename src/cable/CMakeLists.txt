# Action Cable core (architecture section 10). Library: campfire_cable. Tests: test_cable.
find_package(ZLIB REQUIRED)

campfire_library(cable
  SOURCES
    channel.cpp
    connection.cpp
    frame.cpp
    hub.cpp
    protocol.cpp
    websocket.cpp
  PUBLIC_DEPS campfire_core campfire_compat
  DEPS ZLIB::ZLIB
  WERROR)

campfire_test(cable
  SOURCES
    tests/golden_test.cpp
    tests/hub_test.cpp
    tests/protocol_test.cpp
    tests/websocket_edge_test.cpp
    tests/websocket_test.cpp
  WERROR)

campfire_executable(bench_cable_broadcast SOURCES bench/bench_broadcast.cpp DEPS campfire_cable)

# Fuzz targets (architecture section 14): built only with -DCAMPFIRE_FUZZ=ON.
option(CAMPFIRE_FUZZ "Build the libFuzzer targets" OFF)
if(CAMPFIRE_FUZZ)
  add_subdirectory(fuzz)
endif()
