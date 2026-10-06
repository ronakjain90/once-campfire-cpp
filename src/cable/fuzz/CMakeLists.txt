# libFuzzer targets. Configure with -DCAMPFIRE_FUZZ=ON on the asan preset:
#   cmake --preset asan -B /build/fuzz -DCAMPFIRE_FUZZ=ON && cmake --build /build/fuzz
foreach(target frame connection)
  add_executable(fuzz_cable_${target} fuzz_${target}.cpp)
  target_link_libraries(fuzz_cable_${target} PRIVATE campfire_options campfire_cable)
  target_compile_options(fuzz_cable_${target} PRIVATE -fsanitize=fuzzer-no-link)
  target_link_options(fuzz_cable_${target} PRIVATE -fsanitize=fuzzer)
endforeach()
