// Tests of core/xxh3.hpp.
#include "core/xxh3.hpp"

#include <doctest.h>

#include <string>

using namespace campfire;

TEST_CASE("xxh3_128 known values") {
  // Values from `xxhsum -H2` of xxhash 0.8.
  CHECK(xxh3_128("").hex() == "99aa06d3014798d86001c324468d497f");
}

TEST_CASE("streaming equals one shot") {
  std::string data;
  for (int i = 0; i < 5000; ++i) {
    data += static_cast<char>('a' + i % 26);
  }
  for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{16}, std::size_t{17}, std::size_t{240},
                              std::size_t{241}, std::size_t{1024}, data.size()}) {
    const std::string_view part = std::string_view(data).substr(0, n);
    Xxh3State state;
    state.update(part.substr(0, n / 3));
    state.update(part.substr(n / 3));
    CHECK(state.digest() == xxh3_128(part));
  }
}

TEST_CASE("seed, reset and update_u64") {
  CHECK_FALSE(xxh3_128("abc", 1) == xxh3_128("abc", 0));
  Xxh3State state(5);
  state.update("abc");
  state.reset(5);
  state.update("abc");
  CHECK(state.digest() == xxh3_128("abc", 5));
  Xxh3State a;
  a.update_u64(0x0807060504030201ULL);
  CHECK(a.digest() == xxh3_128("\x01\x02\x03\x04\x05\x06\x07\x08"));
  CHECK(xxh3_128("a").hex().size() == 32);
}
