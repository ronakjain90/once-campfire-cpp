// Tests of core/time_format.hpp and core/timestamp.hpp. Expected values come from the Rust tests
// (crates/db/src/time.rs, crates/kit/src/clock.rs, crates/rails_compat/src/metadata.rs,
// crates/campfire/src/controllers/presenters.rs) and from Rails 8.2 (Time#to_fs, #iso8601, #httpdate).
#include "core/time_format.hpp"

#include <doctest.h>

using namespace campfire;

namespace {
Timestamp at(const char* text) {
  const Result<Timestamp> t = parse_rfc3339(text);
  REQUIRE(t.has_value());
  return *t;
}
}  // namespace

TEST_CASE("civil conversion") {
  CHECK(to_civil(Timestamp{0, 0}) == Civil{1970, 1, 1, 0, 0, 0, 4});
  const Civil c = to_civil(Timestamp{1717243200, 0});
  CHECK(c == Civil{2024, 6, 1, 12, 0, 0, 6});  // Saturday
  CHECK(to_civil(Timestamp{-1, 0}) == Civil{1969, 12, 31, 23, 59, 59, 3});
  CHECK(to_civil(Timestamp{kMinSeconds, 0}).year == 1);
  CHECK(to_civil(Timestamp{kMaxSeconds, 0}) == Civil{9999, 12, 31, 23, 59, 59, 5});
  CHECK(to_civil(*from_civil(2024, 2, 29, 1, 2, 3)).day == 29);
  CHECK_FALSE(from_civil(2023, 2, 29, 0, 0, 0).has_value());
  CHECK_FALSE(from_civil(2023, 13, 1, 0, 0, 0).has_value());
  CHECK(Timestamp::from_micros(-500000) == Timestamp{-1, 500000000});
  CHECK(Timestamp::from_nanos(-1) == Timestamp{-1, 999999999});
}

TEST_CASE("to_fs(:number) and cache version") {
  CHECK(format_to_fs_number(at("2024-06-01T12:00:00Z")) == "20240601120000");
  CHECK(format_cache_version(at("2024-06-01T12:00:00.123456789Z")) == "20240601120000123456");
  CHECK(format_cache_version(at("2024-06-01T12:00:00Z")) == "20240601120000000000");
}

TEST_CASE("iso8601 forms") {
  CHECK(format_iso8601(at("2024-06-01T12:00:00.999Z")) == "2024-06-01T12:00:00Z");
  CHECK(format_iso8601_millis(at("2026-01-01T12:00:00.1239Z")) == "2026-01-01T12:00:00.123Z");  // cut, not rounded
  CHECK(format_iso8601_millis(at("2046-01-01T12:00:00Z")) == "2046-01-01T12:00:00.000Z");
  CHECK(format_iso8601_millis(Timestamp::from_micros(-500000)) == "1969-12-31T23:59:59.500Z");
}

TEST_CASE("database text, as Active Record writes it") {
  CHECK(format_db(at("2024-06-01T12:00:00Z")) == "2024-06-01 12:00:00");
  CHECK(format_db(at("2024-06-01T12:00:00.5Z")) == "2024-06-01 12:00:00.500000");
  CHECK(format_db(at("2024-06-01T12:00:00.000123456Z")) == "2024-06-01 12:00:00.000123");
  CHECK(format_db(at("2024-06-01T12:00:00.0000004Z")) == "2024-06-01 12:00:00");  // under 1 us
}

TEST_CASE("parse_db") {
  CHECK(parse_db("2024-06-01 12:00:00") == at("2024-06-01T12:00:00Z"));
  CHECK(parse_db("2024-06-01 12:00:00.123456") == at("2024-06-01T12:00:00.123456Z"));
  CHECK(parse_db("2024-06-01 12:00:00.123") == at("2024-06-01T12:00:00.123Z"));
  CHECK(parse_db("2024-06-01 12:00:00.1234567") == at("2024-06-01T12:00:00.123456Z"));  // 6 digits only
  CHECK(parse_db("2024-06-01T12:00:00Z") == at("2024-06-01T12:00:00Z"));
  CHECK(parse_db("2024-06-01 12:00:00 UTC") == at("2024-06-01T12:00:00Z"));
  CHECK(parse_db(" 2024-06-01 12:00:00 ") == at("2024-06-01T12:00:00Z"));
  CHECK_FALSE(parse_db("").has_value());
  CHECK_FALSE(parse_db("2024-06-01").has_value());
  CHECK_FALSE(parse_db("2024-06-01 12:00:00.").has_value());
  CHECK_FALSE(parse_db("2024-06-01 12:00:00x").has_value());
  CHECK_FALSE(parse_db("2024-13-01 12:00:00").has_value());
  const Timestamp t = Timestamp::from_micros(1717243200123456);
  CHECK(parse_db(format_db(t)) == t);
}

TEST_CASE("http dates") {
  CHECK(format_httpdate(Timestamp{0, 0}) == "Thu, 01 Jan 1970 00:00:00 GMT");
  CHECK(format_httpdate(at("2024-06-01T12:00:00Z")) == "Sat, 01 Jun 2024 12:00:00 GMT");
  CHECK(parse_httpdate("Thu, 01 Jan 1970 00:00:00 GMT") == Timestamp{0, 0});
  CHECK(parse_httpdate("Sat, 01 Jun 2024 12:00:00 GMT") == at("2024-06-01T12:00:00Z"));
  CHECK(parse_httpdate("01 Jun 2024 12:00:00 +0200") == at("2024-06-01T10:00:00Z"));
  CHECK(parse_httpdate("Sat, 01 Jun 24 12:00 EST") == at("2024-06-01T17:00:00Z"));
  CHECK_FALSE(parse_httpdate("Mon, 01 Jun 2024 12:00:00 GMT").has_value());  // wrong weekday
  CHECK_FALSE(parse_httpdate("garbage").has_value());
  CHECK_FALSE(parse_httpdate("").has_value());
  CHECK_FALSE(parse_httpdate("Sat, 32 Jun 2024 12:00:00 GMT").has_value());
}

TEST_CASE("epoch_ms goes through a double, as Ruby does") {
  CHECK(epoch_ms(at("2024-06-01T12:00:00Z")) == 1717243200000);
  CHECK(epoch_ms(at("2024-06-01T12:00:00.123Z")) == 1717243200123);
  // The comment in crates/views/src/messages/support.rs: some values lose 1 ms.
  CHECK(epoch_ms(Timestamp{0, 1'000'000}) == 1);
  CHECK(epoch_ms(Timestamp::from_micros(-500000)) == -500);
  CHECK(epoch_ms(Timestamp{-1, 999'999'999}) == 0);
}

TEST_CASE("RFC 3339 parser") {
  CHECK(at("2024-06-01T12:00:00Z") == Timestamp{1717243200, 0});
  CHECK(at("2024-06-01t12:00:00z") == Timestamp{1717243200, 0});
  CHECK(at("2024-06-01 12:00:00Z") == Timestamp{1717243200, 0});
  CHECK(at("2024-06-01T14:00:00+02:00") == Timestamp{1717243200, 0});
  CHECK(at("2024-06-01T07:00:00-0500") == Timestamp{1717243200, 0});
  CHECK(at("2024-06-01T12:00:00.123456789Z") == Timestamp{1717243200, 123456789});
  CHECK(at("  2024-06-01T12:00:00Z  ") == Timestamp{1717243200, 0});
  CHECK(at("2024-06-01T12:00:60Z") == Timestamp{1717243259, 0});  // leap second clamps to :59
  for (const char* bad :
       {"", "2024-06-01", "2024-06-01T12:00:00", "yesterday", "2024-13-01T00:00:00Z", "2024-06-01T12:00:00Zjunk",
        "2024-06-01T12:00:00.Z", "2024-06-01T12:00:00.1234567890Z", "2024-02-30T00:00:00Z"}) {
    INFO(bad);
    CHECK_FALSE(parse_rfc3339(bad).has_value());
  }
}

TEST_CASE("years_from") {
  CHECK(format_iso8601(years_from(at("2024-02-29T00:00:00Z"), 20)) == "2044-02-29T00:00:00Z");
  CHECK(format_iso8601(years_from(at("2024-02-29T00:00:00Z"), 1)) == "2025-02-28T00:00:00Z");
  CHECK(years_from(at("2024-02-29T00:00:00Z"), 9000) == at("2024-02-29T00:00:00Z"));  // out of range
}
