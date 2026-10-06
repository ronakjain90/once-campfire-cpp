# Request parts (architecture section 4): params, multipart, cookies, session, forgery, formats, bcrypt.
# Library: campfire_req. Tests: test_req. Fuzz targets: fuzz/ (only with -fsanitize=fuzzer).
add_subdirectory("${PROJECT_SOURCE_DIR}/vendor/crypt_blowfish" "${CMAKE_BINARY_DIR}/vendor/crypt_blowfish")

campfire_library(req
  SOURCES
    bcrypt.cpp
    body.cpp
    cookie.cpp
    forgery.cpp
    format.cpp
    multipart.cpp
    param.cpp
    query.cpp
    session.cpp
  PUBLIC_DEPS campfire_core campfire_compat
  DEPS campfire_crypt_blowfish
  WERROR)

set(CAMPFIRE_VECTORS_DIR "${PROJECT_SOURCE_DIR}/spec/vectors" CACHE PATH "Directory of the golden vector JSON files")

campfire_test(req
  SOURCES
    tests/body_test.cpp
    tests/cookie_test.cpp
    tests/forgery_bcrypt_test.cpp
    tests/format_test.cpp
    tests/multipart_test.cpp
    tests/param_test.cpp
  WERROR)
target_compile_definitions(test_req PRIVATE CAMPFIRE_VECTORS_DIR="${CAMPFIRE_VECTORS_DIR}")
target_compile_definitions(test_req PRIVATE CAMPFIRE_PARAMS_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/params_vectors.json")

# Fuzz targets (architecture section 14): built only with -DCAMPFIRE_FUZZ=ON.
option(CAMPFIRE_FUZZ "Build the libFuzzer targets" OFF)
if(CAMPFIRE_FUZZ)
  add_subdirectory(fuzz)
endif()
