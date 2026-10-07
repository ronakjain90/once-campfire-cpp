# Path and URL helpers of config/routes.rb. Library: campfire_routes. Tests: test_routes.
campfire_library(routes
  SOURCES routes.cpp
  PUBLIC_DEPS campfire_compat
  WERROR)

set(CAMPFIRE_VECTORS_DIR "${PROJECT_SOURCE_DIR}/tests/vectors" CACHE PATH "Directory of the golden vector JSON files")

campfire_test(routes
  SOURCES test/routes_test.cpp
  WERROR)
target_compile_definitions(test_routes PRIVATE
  CAMPFIRE_VECTORS_DIR="${CAMPFIRE_VECTORS_DIR}"
  CAMPFIRE_ROUTES_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test")
