# Rich text core: Gumbo, the DOM, the serializer and the sanitizer. Task T3.
file(GLOB RICHTEXT_GUMBO_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/gumbo/*.c")

# Gumbo is C code with its own warnings policy: build it as a separate library with no warnings.
add_library(campfire_gumbo STATIC ${RICHTEXT_GUMBO_SOURCES})
target_include_directories(campfire_gumbo SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/gumbo")
target_compile_options(campfire_gumbo PRIVATE -w)
set_target_properties(campfire_gumbo PROPERTIES C_STANDARD 11)
# Sanitizer flags must reach Gumbo too.
target_link_libraries(campfire_gumbo PRIVATE campfire_options)

campfire_library(richtext
  SOURCES dom.cpp sanitizer.cpp filters.cpp text_util.cpp tree.cpp uri.cpp attachables.cpp plain_text.cpp
          content.cpp message_filters.cpp autolink.cpp richtext.cpp resolver.cpp
  PUBLIC_DEPS campfire_gumbo campfire_core campfire_compat)
campfire_test(richtext
  SOURCES tests/dom_test.cpp tests/sanitizer_test.cpp tests/corpus_test.cpp)
target_compile_definitions(test_richtext PRIVATE
  RICHTEXT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
