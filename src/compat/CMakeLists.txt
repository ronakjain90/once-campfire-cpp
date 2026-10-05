# Rails and Ruby compatibility (architecture section 8). Library: campfire_compat. Tests: test_compat.
find_package(OpenSSL REQUIRED)

add_library(campfire_compat STATIC
  base64.cpp
  content_disposition.cpp
  cookies.cpp
  crypto.cpp
  global_id.cpp
  json.cpp
  marshal.cpp
  message_encryptor.cpp
  message_verifier.cpp
  metadata.cpp
  ruby.cpp
  ruby_float.cpp
  secrets.cpp
  signed_id.cpp
  time.cpp
  turbo.cpp
  variation.cpp)
target_compile_features(campfire_compat PUBLIC cxx_std_23)
# Headers are included as "compat/<name>.hpp".
target_include_directories(campfire_compat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(campfire_compat PUBLIC OpenSSL::Crypto)
target_compile_options(campfire_compat PRIVATE -Wall -Wextra)

# Golden vectors: the Rust repo's vectors/, copied to spec/vectors/ by T1.
set(CAMPFIRE_VECTORS_DIR "${CMAKE_SOURCE_DIR}/spec/vectors" CACHE PATH "Directory of the golden vector JSON files")
# doctest.h: found on the include path, or in CAMPFIRE_DOCTEST_DIR.
set(CAMPFIRE_DOCTEST_DIR "" CACHE PATH "Directory that holds doctest/doctest.h or doctest.h")

add_executable(test_compat
  test/main.cpp
  test/test_ruby.cpp
  test/test_rails.cpp
  test/test_storage.cpp
  test/test_units.cpp)
target_link_libraries(test_compat PRIVATE campfire_compat)
target_include_directories(test_compat PRIVATE ${CAMPFIRE_DOCTEST_DIR})
target_compile_definitions(test_compat PRIVATE CAMPFIRE_VECTORS_DIR="${CAMPFIRE_VECTORS_DIR}")
add_test(NAME test_compat COMMAND test_compat)
