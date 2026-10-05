# Top-level build of the Campfire C++ port. See AGENTS.md for the helper functions.
cmake_minimum_required(VERSION 3.25)
project(campfire LANGUAGES C CXX)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CampfireOptions)
include(CampfireHelpers)

enable_testing()

add_subdirectory(vendor)
add_subdirectory(tests)

# Add each src/<dir> that has a CMakeLists.txt, in alphabetical order. A library that depends
# on another one must name it in DEPS. CMake resolves the order of the link.
file(GLOB campfire_src_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*")
list(SORT campfire_src_dirs)
foreach(dir IN LISTS campfire_src_dirs)
  if(IS_DIRECTORY "${dir}" AND EXISTS "${dir}/CMakeLists.txt")
    get_filename_component(name "${dir}" NAME)
    message(STATUS "campfire: adding src/${name}")
    add_subdirectory("${dir}")
  endif()
endforeach()
