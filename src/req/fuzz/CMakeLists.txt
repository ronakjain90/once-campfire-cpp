# libFuzzer targets. Configure with -DCAMPFIRE_FUZZ=ON on the asan preset:
#   cmake --preset asan -B /build/fuzz -DCAMPFIRE_FUZZ=ON && cmake --build /build/fuzz
foreach(target query multipart cookie accept)
  add_executable(fuzz_req_${target} fuzz_${target}.cpp)
  target_link_libraries(fuzz_req_${target} PRIVATE campfire_options campfire_req)
  target_compile_options(fuzz_req_${target} PRIVATE -fsanitize=fuzzer-no-link)
  target_link_options(fuzz_req_${target} PRIVATE -fsanitize=fuzzer)
endforeach()
