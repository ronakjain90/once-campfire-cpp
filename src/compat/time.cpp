// Timestamps for message expiry (see time.hpp).
#include "compat/time.hpp"

#include <cstdio>

namespace campfire::compat {
namespace chr = std::chrono;

std::string iso8601_millis(Timestamp time) {
  auto ns = time.time_since_epoch();
  auto secs = chr::floor<chr::seconds>(ns);
  int64_t millis = chr::duration_cast<chr::milliseconds>(ns - secs).count();
  chr::sys_days day = chr::floor<chr::days>(chr::sys_seconds(secs));
  chr::year_month_day ymd{day};
  auto in_day = chr::sys_seconds(secs) - day;
  int64_t s = in_day.count();
  char buf[48];
  std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ", int(ymd.year()), unsigned(ymd.month()),
                unsigned(ymd.day()), int(s / 3600), int(s / 60 % 60), int(s % 60), int(millis));
  return buf;
}

namespace {

bool digits(std::string_view s, size_t pos, size_t count, int& out) {
  if (pos + count > s.size()) return false;
  int value = 0;
  for (size_t i = 0; i < count; ++i) {
    char c = s[pos + i];
    if (c < '0' || c > '9') return false;
    value = value * 10 + (c - '0');
  }
  out = value;
  return true;
}

}  // namespace

std::optional<Timestamp> parse_iso8601(std::string_view s) {
  int year, month, day, hour, minute, second;
  if (!digits(s, 0, 4, year) || s.size() < 19 || s[4] != '-' || !digits(s, 5, 2, month) || s[7] != '-' ||
      !digits(s, 8, 2, day) || s[10] != 'T' || !digits(s, 11, 2, hour) || s[13] != ':' || !digits(s, 14, 2, minute) ||
      s[16] != ':' || !digits(s, 17, 2, second)) {
    return std::nullopt;
  }
  if (hour > 23 || minute > 59 || second > 59) return std::nullopt;
  chr::year_month_day ymd{chr::year(year), chr::month(unsigned(month)), chr::day(unsigned(day))};
  if (!ymd.ok()) return std::nullopt;
  size_t pos = 19;
  int64_t nanos = 0;
  if (pos < s.size() && s[pos] == '.') {
    ++pos;
    size_t start = pos;
    int64_t scale = 100000000;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
      if (scale > 0) {
        nanos += (s[pos] - '0') * scale;
        scale /= 10;
      }
      ++pos;
    }
    if (pos == start) return std::nullopt;
  }
  int64_t offset_seconds = 0;
  if (pos < s.size() && s[pos] == 'Z') {
    ++pos;
  } else if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
    int sign = s[pos] == '+' ? 1 : -1;
    int oh, om;
    if (!digits(s, pos + 1, 2, oh) || pos + 3 >= s.size() || s[pos + 3] != ':' || !digits(s, pos + 4, 2, om)) {
      return std::nullopt;
    }
    if (oh > 23 || om > 59) return std::nullopt;
    offset_seconds = sign * (oh * 3600 + om * 60);
    pos += 6;
  } else {
    return std::nullopt;
  }
  if (pos != s.size()) return std::nullopt;
  chr::sys_days days{ymd};
  auto total = chr::seconds(days.time_since_epoch()) + chr::hours(hour) + chr::minutes(minute) + chr::seconds(second) -
               chr::seconds(offset_seconds);
  return Timestamp(chr::duration_cast<chr::nanoseconds>(total) + chr::nanoseconds(nanos));
}

Timestamp permanent_expires_at(Timestamp now) {
  auto secs = chr::floor<chr::seconds>(now.time_since_epoch());
  auto rest = now.time_since_epoch() - secs;
  chr::sys_days day = chr::floor<chr::days>(chr::sys_seconds(secs));
  auto in_day = chr::sys_seconds(secs) - day;
  chr::year_month_day ymd{day};
  chr::year_month_day later{ymd.year() + chr::years(20), ymd.month(), ymd.day()};
  if (!later.ok()) later = later.year() / later.month() / chr::last;
  return Timestamp(chr::duration_cast<chr::nanoseconds>(chr::sys_days(later).time_since_epoch() + in_day) + rest);
}

}  // namespace campfire::compat
