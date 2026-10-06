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
    tests/websocket_test.cpp
  WERROR)
