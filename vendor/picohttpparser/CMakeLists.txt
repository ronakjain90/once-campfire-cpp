# Vendored picohttpparser (MIT or Perl license, see the header of picohttpparser.c).
add_library(campfire_picohttpparser STATIC picohttpparser.c)
target_include_directories(campfire_picohttpparser SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(campfire_picohttpparser PRIVATE -w)
target_link_libraries(campfire_picohttpparser PRIVATE campfire_options)
