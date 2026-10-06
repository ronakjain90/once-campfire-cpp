# Rails and Ruby compatibility (architecture section 8). Library: campfire_compat. Tests: test_compat.
find_package(OpenSSL REQUIRED)

campfire_library(compat
  SOURCES
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
    variation.cpp
  PUBLIC_DEPS OpenSSL::Crypto
  WERROR)

# Golden vectors: the Rust repo's vectors/, copied to spec/vectors/.
set(CAMPFIRE_VECTORS_DIR "${PROJECT_SOURCE_DIR}/spec/vectors" CACHE PATH "Directory of the golden vector JSON files")

campfire_test(compat
  SOURCES
    test/test_ruby.cpp
    test/test_rails.cpp
    test/test_storage.cpp
    test/test_units.cpp
  WERROR)
target_compile_definitions(test_compat PRIVATE CAMPFIRE_VECTORS_DIR="${CAMPFIRE_VECTORS_DIR}")
