# Core library: arena, buffers, errors, log, config, clock, hashes, time formats, Task.
campfire_library(core
  SOURCES
    clock.cpp
    config.cpp
    html.cpp
    log.cpp
    out.cpp
    time_format.cpp
    xxh3.cpp
  PUBLIC_DEPS Threads::Threads
  DEPS campfire_xxhash
  WERROR)
target_compile_options(campfire_core PRIVATE -Wconversion -Wsign-conversion)

campfire_test(core
  SOURCES
    tests/arena_test.cpp
    tests/clock_test.cpp
    tests/config_test.cpp
    tests/error_log_test.cpp
    tests/html_test.cpp
    tests/out_test.cpp
    tests/task_test.cpp
    tests/time_format_test.cpp
    tests/xxh3_test.cpp
  WERROR)
