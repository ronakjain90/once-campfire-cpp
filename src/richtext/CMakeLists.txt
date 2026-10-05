# Rich text core: Gumbo, the DOM, the serializer and the sanitizer. Task T3.
# With T1's helpers (campfire_library, campfire_test) the targets use them. Without them, plain CMake.
set(RICHTEXT_SOURCES dom.cpp sanitizer.cpp filters.cpp)
set(RICHTEXT_TEST_SOURCES tests/dom_test.cpp tests/sanitizer_test.cpp tests/corpus_test.cpp)
file(GLOB RICHTEXT_GUMBO_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/gumbo/*.c")

# Gumbo is C code with its own warnings policy: build it as a separate library with no warnings.
add_library(campfire_gumbo STATIC ${RICHTEXT_GUMBO_SOURCES})
target_include_directories(campfire_gumbo PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/gumbo")
target_compile_options(campfire_gumbo PRIVATE -w)
set_target_properties(campfire_gumbo PROPERTIES C_STANDARD 11)
if(TARGET campfire_options)
  # Sanitizer flags must reach Gumbo too.
  target_link_libraries(campfire_gumbo PRIVATE campfire_options)
endif()

if(COMMAND campfire_library)
  campfire_library(richtext SOURCES ${RICHTEXT_SOURCES} PUBLIC_DEPS campfire_gumbo WERROR)
  campfire_test(richtext SOURCES ${RICHTEXT_TEST_SOURCES} WERROR)
else()
  add_library(campfire_richtext STATIC ${RICHTEXT_SOURCES})
  target_include_directories(campfire_richtext PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
  target_link_libraries(campfire_richtext PUBLIC campfire_gumbo)
  target_compile_options(campfire_richtext PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Werror)
  add_executable(test_richtext ${RICHTEXT_TEST_SOURCES})
  target_link_libraries(test_richtext PRIVATE campfire_richtext)
  add_test(NAME test_richtext COMMAND test_richtext)
endif()
target_include_directories(campfire_richtext PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_definitions(test_richtext PRIVATE
  RICHTEXT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
