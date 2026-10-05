// Tests of core/clock.hpp. Rust: crates/rails_compat/src/clock.rs and crates/kit/src/clock.rs.
#include "core/clock.hpp"

#include <doctest.h>

#include <map>

#include "core/time_format.hpp"

using namespace campfire;

namespace {
Result<SharedClock> from(std::map<std::string, std::string> vars) {
  return clock_from_lookup([vars](std::string_view name) -> std::optional<std::string> {
    const auto it = vars.find(std::string(name));
    if (it == vars.end()) {
      return std::nullopt;
    }
    return it->second;
  });
}
}  // namespace

TEST_CASE("a frozen clock moves only when told") {
  const Timestamp t = *parse_rfc3339("2024-06-01T12:00:00Z");
  const auto clock = TestClock::frozen_at(t);
  CHECK(clock->now() == t);
  clock->travel(60);
  CHECK(format_iso8601(clock->now()) == "2024-06-01T12:01:00Z");
  clock->travel_to(t);
  CHECK(clock->now() == t);
}

TEST_CASE("travel shifts the real time until travel_back") {
  TestClock clock;
  const Timestamp real = SystemClock{}.now();
  clock.travel(3600);
  const std::int64_t shifted = clock.now().seconds - real.seconds;
  CHECK(shifted >= 3599);
  CHECK(shifted <= 3601);
  clock.travel_back();
  CHECK(clock.now().seconds - SystemClock{}.now().seconds <= 1);
}

TEST_CASE("the system clock is near the real time") {
  const Timestamp now = SystemClock{}.now();
  CHECK(now.seconds > 1'700'000'000);
  CHECK(now.nanos >= 0);
  CHECK(now.nanos < 1'000'000'000);
}

TEST_CASE("CAMPFIRE_FROZEN_TIME") {
  const auto frozen = from({{"CAMPFIRE_FROZEN_TIME", " 2024-06-01T12:00:00Z "}});
  REQUIRE(frozen.has_value());
  CHECK((*frozen)->now() == Timestamp{1717243200, 0});
  CHECK((*frozen)->now() == Timestamp{1717243200, 0});

  for (const auto& vars : {std::map<std::string, std::string>{}, {{"CAMPFIRE_FROZEN_TIME", ""}}, {{"CAMPFIRE_FROZEN_TIME", "  "}}}) {
    const auto real = from(vars);
    REQUIRE(real.has_value());
    CHECK((*real)->now().seconds > 1'700'000'000);
  }

  const auto bad = from({{"CAMPFIRE_FROZEN_TIME", "tomorrow"}});
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().code == Errc::Config);
  CHECK(bad.error().message.starts_with("CAMPFIRE_FROZEN_TIME=\"tomorrow\" is not an RFC 3339 timestamp"));
}
