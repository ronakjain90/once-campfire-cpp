# libFuzzer targets of the Rails compatibility code. Configure with -DCAMPFIRE_FUZZ=ON on the asan preset.
foreach(target json marshal_base64 messages)
  add_executable(fuzz_compat_${target} fuzz_${target}.cpp)
  target_link_libraries(fuzz_compat_${target} PRIVATE campfire_options campfire_compat)
  target_link_options(fuzz_compat_${target} PRIVATE -fsanitize=fuzzer)
endforeach()
