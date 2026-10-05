// Tests of core/arena.hpp.
#include "core/arena.hpp"

#include <doctest.h>

#include <memory_resource>
#include <string>

using campfire::Arena;

TEST_CASE("Arena copies text and keeps it valid") {
  Arena arena(64);
  std::string source = "hello";
  const std::string_view copy = arena.copy(source);
  source = "xxxxx";
  CHECK(copy == "hello");
  CHECK(arena.copy("").empty());
  // Many allocations force the arena to grow beyond its first block.
  std::pmr::vector<std::string_view> views{arena.resource()};
  for (int i = 0; i < 1000; ++i) {
    views.push_back(arena.copy(std::to_string(i)));
  }
  CHECK(views[999] == "999");
  CHECK(copy == "hello");
}

TEST_CASE("Arena::make aligns and constructs") {
  Arena arena;
  struct alignas(32) Wide {
    int value;
  };
  (void)arena.make<char>('a');
  Wide* wide = arena.make<Wide>(Wide{7});
  CHECK(wide->value == 7);
  CHECK(reinterpret_cast<std::uintptr_t>(wide) % 32 == 0);
}

TEST_CASE("Arena::reset allows reuse") {
  Arena arena(32);
  for (int round = 0; round < 3; ++round) {
    CHECK(arena.copy(std::string(500, 'a')).size() == 500);
    arena.reset();
  }
}
