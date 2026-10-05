// Tests of core/error.hpp and core/log.hpp.
#include <doctest.h>

#include <string>
#include <vector>

#include "core/error.hpp"
#include "core/log.hpp"

using namespace campfire;

namespace {
Result<int> parse_positive(int v) {
  if (v <= 0) {
    return fail(Errc::InvalidArgument, "not positive");
  }
  return v;
}
Status check(int v) {
  const Result<int> r = parse_positive(v);
  if (!r) {
    return std::unexpected(r.error());
  }
  return {};
}
}  // namespace

TEST_CASE("Result and Status carry values and errors") {
  CHECK(*parse_positive(3) == 3);
  const Result<int> bad = parse_positive(-1);
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().code == Errc::InvalidArgument);
  CHECK(bad.error().message == "not positive");
  CHECK(check(1).has_value());
  CHECK_FALSE(check(0).has_value());
  CHECK(to_string(Errc::Config) == "config");
  CHECK(Error{Errc::Io, "x"} == Error{Errc::Io, "x"});
}

TEST_CASE("log level names") {
  CHECK(parse_log_level("debug") == LogLevel::Debug);
  CHECK(parse_log_level("INFO") == LogLevel::Info);
  CHECK(parse_log_level("Warn") == LogLevel::Warn);
  CHECK(parse_log_level("error") == LogLevel::Error);
  CHECK(parse_log_level("fatal") == LogLevel::Fatal);
  CHECK(parse_log_level("unknown") == LogLevel::Unknown);
  CHECK_FALSE(parse_log_level("verbose").has_value());
  CHECK_FALSE(parse_log_level("").has_value());
}

TEST_CASE("the logger filters by level and formats lines") {
  Logger& logger = Logger::instance();
  std::vector<std::string> lines;
  logger.set_sink([&](LogLevel, std::string_view line) { lines.emplace_back(line); });
  logger.set_level(LogLevel::Warn);
  log_info("hidden {}", 1);
  log_warn("shown {} {}", 2, "x");
  log_error("also shown");
  REQUIRE(lines.size() == 2);
  // 2026-01-01T12:00:00.123Z WARN shown 2 x
  CHECK(lines[0].size() > 24);
  CHECK(lines[0].substr(24) == " WARN shown 2 x");
  CHECK(lines[0][10] == 'T');
  CHECK(lines[1].substr(24) == " ERROR also shown");

  CHECK(logger.set_level_from("debug"));
  CHECK(logger.level() == LogLevel::Debug);
  CHECK_FALSE(logger.set_level_from("loud"));
  CHECK(logger.level() == LogLevel::Debug);
  logger.set_sink({});
  logger.set_level(LogLevel::Info);
}
