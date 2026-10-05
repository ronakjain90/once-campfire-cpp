# Database library: connections, typed statements, writer, checkpointer, generated schema.
add_subdirectory("${PROJECT_SOURCE_DIR}/vendor/sqlite" "${CMAKE_BINARY_DIR}/vendor/sqlite")

# The row structs come from spec/schema.rb. The output is in the source tree too (committed), so
# a build without Python still works; the build regenerates it when schema.rb changes.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/schema.gen.hpp"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/schema_gen.py"
            "${PROJECT_SOURCE_DIR}/spec/schema.rb" "${CMAKE_CURRENT_SOURCE_DIR}/schema.gen.hpp"
    DEPENDS "${PROJECT_SOURCE_DIR}/spec/schema.rb" "${PROJECT_SOURCE_DIR}/tools/schema_gen.py"
    COMMENT "schema_gen: spec/schema.rb -> src/db/schema.gen.hpp")
  add_custom_target(campfire_db_schema DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/schema.gen.hpp")
endif()

campfire_library(db
  SOURCES
    connection.cpp
    database.cpp
  PUBLIC_DEPS campfire_core campfire_sqlite3
  WERROR)
target_compile_options(campfire_db PRIVATE -Wconversion -Wsign-conversion)
if(TARGET campfire_db_schema)
  add_dependencies(campfire_db campfire_db_schema)
endif()

campfire_test(db
  SOURCES
    tests/connection_test.cpp
    tests/scope_test.cpp
    tests/writer_test.cpp
  WERROR)
campfire_executable(db_bench SOURCES bench/db_bench.cpp DEPS campfire_db)
