// A point in time with nanosecond precision, in UTC. Matches the use of jiff::Timestamp in the
// Rust port (crates/rails_compat/src/clock.rs) and of Time in Rails.
#pragma once

#include <compare>
#include <cstdint>
#include <optional>

namespace campfire {

// A broken-down UTC date and time. `weekday` is 0 for Sunday.
struct Civil {
  int year = 1970;
  int month = 1;   // 1 to 12
  int day = 1;     // 1 to 31
  int hour = 0;    // 0 to 23
  int minute = 0;  // 0 to 59
  int second = 0;  // 0 to 59
  int weekday = 4;
  [[nodiscard]] bool operator==(const Civil&) const = default;
};

// Seconds since 1970-01-01T00:00:00Z, and a nanosecond part in the range 0 to 999,999,999.
// Before 1970 the nanosecond part is still positive: -0.5 s is {-1, 500000000}.
struct Timestamp {
  std::int64_t seconds = 0;
  std::int32_t nanos = 0;

  [[nodiscard]] static constexpr Timestamp from_seconds(std::int64_t s) noexcept { return {s, 0}; }
  [[nodiscard]] static constexpr Timestamp from_micros(std::int64_t us) noexcept {
    std::int64_t s = us / 1'000'000;
    std::int64_t rest = us % 1'000'000;
    if (rest < 0) {
      rest += 1'000'000;
      --s;
    }
    return {s, static_cast<std::int32_t>(rest * 1000)};
  }
  [[nodiscard]] static constexpr Timestamp from_nanos(std::int64_t ns) noexcept {
    std::int64_t s = ns / 1'000'000'000;
    std::int64_t rest = ns % 1'000'000'000;
    if (rest < 0) {
      rest += 1'000'000'000;
      --s;
    }
    return {s, static_cast<std::int32_t>(rest)};
  }

  [[nodiscard]] constexpr Timestamp plus_seconds(std::int64_t s) const noexcept { return {seconds + s, nanos}; }
  [[nodiscard]] constexpr Timestamp plus_nanos(std::int64_t ns) const noexcept {
    std::int64_t s = seconds + ns / 1'000'000'000;
    std::int64_t n = nanos + ns % 1'000'000'000;
    if (n >= 1'000'000'000) {
      n -= 1'000'000'000;
      ++s;
    } else if (n < 0) {
      n += 1'000'000'000;
      --s;
    }
    return {s, static_cast<std::int32_t>(n)};
  }
  [[nodiscard]] constexpr std::int32_t micros() const noexcept { return nanos / 1000; }

  [[nodiscard]] constexpr auto operator<=>(const Timestamp&) const noexcept = default;
};

// The valid range is the years 0001 to 9999, as in jiff.
inline constexpr std::int64_t kMinSeconds = -62'135'596'800;  // 0001-01-01T00:00:00Z
inline constexpr std::int64_t kMaxSeconds = 253'402'300'799;  // 9999-12-31T23:59:59Z

// Days since 1970-01-01 of a civil date (proleptic Gregorian calendar).
[[nodiscard]] constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

[[nodiscard]] constexpr bool is_leap_year(int y) noexcept { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

[[nodiscard]] constexpr int days_in_month(int y, int m) noexcept {
  constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && is_leap_year(y) ? 29 : kDays[m - 1];
}

// Breaks `t` into UTC fields.
[[nodiscard]] constexpr Civil to_civil(Timestamp t) noexcept {
  std::int64_t days = t.seconds / 86400;
  std::int64_t rest = t.seconds % 86400;
  if (rest < 0) {
    rest += 86400;
    --days;
  }
  const std::int64_t z = days + 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  Civil c;
  c.year = static_cast<int>(static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2 ? 1 : 0));
  c.month = static_cast<int>(m);
  c.day = static_cast<int>(d);
  c.hour = static_cast<int>(rest / 3600);
  c.minute = static_cast<int>(rest % 3600 / 60);
  c.second = static_cast<int>(rest % 60);
  c.weekday = static_cast<int>(((days % 7) + 11) % 7);  // 1970-01-01 was a Thursday (4)
  return c;
}

// Builds a timestamp from UTC fields. Returns nothing if a field is out of range or the result
// is outside the valid range.
[[nodiscard]] constexpr std::optional<Timestamp> from_civil(int year, int month, int day, int hour, int minute,
                                                            int second, std::int32_t nanos = 0) noexcept {
  if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1 || day > days_in_month(year, month) ||
      hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59 || nanos < 0 ||
      nanos > 999'999'999) {
    return std::nullopt;
  }
  const std::int64_t days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  return Timestamp{days * 86400 + hour * 3600 + minute * 60 + second, nanos};
}

}  // namespace campfire
