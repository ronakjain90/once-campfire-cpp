cmake_minimum_required(VERSION 3.25)
project(campfire_gate C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Flags are exactly the ones the task allows: -O2, LTO, -march=armv8.2-a+crypto. No build type.
set(CMAKE_BUILD_TYPE "")
set(GATE_FLAGS -O2 -flto -march=armv8.2-a+crypto)
add_compile_options(${GATE_FLAGS})
add_link_options(${GATE_FLAGS} -fuse-ld=lld)

# SQLite 3.53.2 amalgamation (the version rusqlite's bundled build uses), same compile options as libsqlite3-sys.
add_library(sqlite3 STATIC vendor/sqlite3.c)
target_compile_definitions(sqlite3 PRIVATE
  SQLITE_CORE SQLITE_DEFAULT_FOREIGN_KEYS=1 SQLITE_ENABLE_API_ARMOR SQLITE_ENABLE_COLUMN_METADATA SQLITE_ENABLE_DBSTAT_VTAB
  SQLITE_ENABLE_FTS3 SQLITE_ENABLE_FTS3_PARENTHESIS SQLITE_ENABLE_FTS5 SQLITE_ENABLE_JSON1 SQLITE_ENABLE_LOAD_EXTENSION=1
  SQLITE_ENABLE_MEMORY_MANAGEMENT SQLITE_ENABLE_RTREE SQLITE_ENABLE_STAT4 SQLITE_SOUNDEX SQLITE_THREADSAFE=1 SQLITE_USE_URI
  HAVE_USLEEP=1 HAVE_ISNAN HAVE_STDINT_H=1)
add_library(picohttpparser STATIC vendor/picohttpparser.c)
include_directories(vendor src)

add_library(gatecrypto STATIC src/crypto.cpp)
target_link_libraries(gatecrypto PUBLIC crypto)

add_executable(gate-server src/main.cpp src/server.cpp src/writer.cpp src/db.cpp)
target_link_libraries(gate-server PRIVATE gatecrypto sqlite3 picohttpparser deflate crypt jemalloc pthread dl m)
add_executable(cookie_test src/cookie_test.cpp)
target_link_libraries(cookie_test PRIVATE gatecrypto)
