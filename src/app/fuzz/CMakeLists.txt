# libFuzzer targets of the link unfurl. Configure with -DCAMPFIRE_FUZZ=ON on the asan preset:
#   cmake --preset asan -B /build/fuzz -DCAMPFIRE_FUZZ=ON && cmake --build /build/fuzz
foreach(target opengraph_html unfurl_response)
  add_executable(fuzz_app_${target} fuzz_${target}.cpp)
  target_link_libraries(fuzz_app_${target} PRIVATE campfire_options campfire_app)
  target_compile_options(fuzz_app_${target} PRIVATE -fsanitize=fuzzer-no-link)
  target_link_options(fuzz_app_${target} PRIVATE -fsanitize=fuzzer)
endforeach()
