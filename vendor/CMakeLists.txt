# Vendored code. Each library is a SYSTEM include path, so its warnings do not show.
add_library(campfire_xxhash INTERFACE)
target_include_directories(campfire_xxhash SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/xxhash")

add_library(campfire_doctest INTERFACE)
target_include_directories(campfire_doctest SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/doctest")
