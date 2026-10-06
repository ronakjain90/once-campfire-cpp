# crypt_blowfish 1.3 (Openwall, public domain), as in the bcrypt gem 3.1.22 that Rails uses.
# The portable C code only: no x86.S. The include path is SYSTEM.
add_library(campfire_crypt_blowfish STATIC crypt_blowfish.c crypt_gensalt.c)
target_include_directories(campfire_crypt_blowfish SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(campfire_crypt_blowfish PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(campfire_crypt_blowfish PRIVATE -w)
