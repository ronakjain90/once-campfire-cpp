# Compiler options of all Campfire targets: one INTERFACE library, campfire_options.

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CAMPFIRE_SANITIZER "" CACHE STRING "Sanitizer build: address, thread, or empty")
set_property(CACHE CAMPFIRE_SANITIZER PROPERTY STRINGS "" address thread)
option(CAMPFIRE_LTO "Link-time optimization" OFF)
option(CAMPFIRE_JEMALLOC "Link jemalloc into the executables" OFF)

add_library(campfire_options INTERFACE)
target_compile_options(campfire_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow
                                                  -fno-omit-frame-pointer)

if(CAMPFIRE_SANITIZER STREQUAL "address")
  set(_san -fsanitize=address,undefined -fno-sanitize-recover=undefined)
elseif(CAMPFIRE_SANITIZER STREQUAL "thread")
  set(_san -fsanitize=thread)
elseif(NOT CAMPFIRE_SANITIZER STREQUAL "")
  message(FATAL_ERROR "CAMPFIRE_SANITIZER must be address, thread, or empty")
endif()
if(_san)
  target_compile_options(campfire_options INTERFACE ${_san})
  target_link_options(campfire_options INTERFACE ${_san})
endif()

if(CAMPFIRE_LTO)
  target_compile_options(campfire_options INTERFACE -flto=thin)
  target_link_options(campfire_options INTERFACE -flto=thin -fuse-ld=lld)
endif()

# CPU baseline. arm64: Apple M-class and Graviton need the crypto extensions for fast AES, SHA
# and CRC. x86-64: the v2 level (SSE4.2, POPCNT) runs on any CPU since 2009.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(CAMPFIRE_ARCH_FLAG -march=armv8.2-a+crypto)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
  set(CAMPFIRE_ARCH_FLAG -march=x86-64-v2)
endif()
if(CAMPFIRE_ARCH_FLAG AND NOT CAMPFIRE_SANITIZER)
  target_compile_options(campfire_options INTERFACE ${CAMPFIRE_ARCH_FLAG})
endif()

# jemalloc: a separate INTERFACE library that executables link. It does nothing in the
# sanitizer builds, because a sanitizer must see every allocation.
add_library(campfire_malloc INTERFACE)
if(CAMPFIRE_JEMALLOC)
  if(CAMPFIRE_SANITIZER)
    message(FATAL_ERROR "CAMPFIRE_JEMALLOC cannot be on in a sanitizer build")
  endif()
  find_library(JEMALLOC_LIBRARY NAMES jemalloc REQUIRED)
  target_link_libraries(campfire_malloc INTERFACE ${JEMALLOC_LIBRARY})
endif()

option(CAMPFIRE_WERROR "Treat warnings as errors in all Campfire targets" OFF)
