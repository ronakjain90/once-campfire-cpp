cd /Volumes/ExternalHD/Code/AI/once-campfire/wt/T5
cmake -S . -B /build/fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O1 -g" -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O1 -g" -DCAMPFIRE_SANITIZER=address -DCAMPFIRE_FUZZ=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR=llvm-ar -DCMAKE_RANLIB=llvm-ranlib >/dev/null && cmake --build /build/fuzz --target fuzz_http_parser 2>&1 | tail -5
mkdir -p /build/fuzz-corpus
/build/fuzz/src/net/fuzz_http_parser -max_total_time=300 -max_len=4096 /build/fuzz-corpus 2>&1 | tail -15
