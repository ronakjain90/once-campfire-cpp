# campfire_library(<dir> SOURCES ... [DEPS ...] [PUBLIC_DEPS ...] [WERROR])
#   Defines the static library campfire_<dir> (alias campfire::<dir>). The public include root
#   is src/, so code writes #include "core/out.hpp". Call it from src/<dir>/CMakeLists.txt.
#
# campfire_test(<dir> SOURCES ... [DEPS ...])
#   Defines the doctest executable test_<dir>, links it with campfire_<dir> (if it exists) and
#   the shared doctest main, and registers it with ctest.
#
# campfire_executable(<name> SOURCES ... [DEPS ...])
#   Defines an executable. It links campfire_malloc (jemalloc in the release build).

function(campfire_library name)
  cmake_parse_arguments(ARG "WERROR" "" "SOURCES;DEPS;PUBLIC_DEPS" ${ARGN})
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "campfire_library(${name}): SOURCES is required")
  endif()
  add_library(campfire_${name} STATIC ${ARG_SOURCES})
  add_library(campfire::${name} ALIAS campfire_${name})
  target_include_directories(campfire_${name} PUBLIC "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(campfire_${name} PUBLIC ${ARG_PUBLIC_DEPS} PRIVATE campfire_options ${ARG_DEPS})
  if(ARG_WERROR OR CAMPFIRE_WERROR)
    target_compile_options(campfire_${name} PRIVATE -Werror)
  endif()
endfunction()

function(campfire_test name)
  cmake_parse_arguments(ARG "WERROR" "" "SOURCES;DEPS" ${ARGN})
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "campfire_test(${name}): SOURCES is required")
  endif()
  add_executable(test_${name} ${ARG_SOURCES})
  target_link_libraries(test_${name} PRIVATE campfire_options campfire_doctest_main campfire_malloc ${ARG_DEPS})
  if(TARGET campfire_${name})
    target_link_libraries(test_${name} PRIVATE campfire_${name})
  endif()
  if(ARG_WERROR OR CAMPFIRE_WERROR)
    target_compile_options(test_${name} PRIVATE -Werror)
  endif()
  add_test(NAME test_${name} COMMAND test_${name})
  # Sanitizer reports must fail the test and must not hide a leak.
  set_tests_properties(test_${name} PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:abort_on_error=0;UBSAN_OPTIONS=print_stacktrace=1;TSAN_OPTIONS=halt_on_error=1")
endfunction()

function(campfire_executable name)
  cmake_parse_arguments(ARG "WERROR" "" "SOURCES;DEPS" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE campfire_options campfire_malloc ${ARG_DEPS})
  if(ARG_WERROR OR CAMPFIRE_WERROR)
    target_compile_options(${name} PRIVATE -Werror)
  endif()
endfunction()
