# libFuzzer targets of the server. Configure with -DCAMPFIRE_FUZZ=ON on the asan preset:
#   cmake --preset asan -B /build/fuzz -DCAMPFIRE_FUZZ=ON && cmake --build /build/fuzz
foreach(target http_parser server misc acme_json)
  add_executable(fuzz_net_${target} fuzz_${target}.cpp)
  target_link_libraries(fuzz_net_${target} PRIVATE campfire_options campfire_net)
  target_link_options(fuzz_net_${target} PRIVATE -fsanitize=fuzzer)
endforeach()
