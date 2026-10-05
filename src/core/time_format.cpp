// Rails time formats. See time_format.hpp for the sources.
#include "core/time_format.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace campfire {
namespace {

void put_padded(std::string& out, std::int64_t value, int width) {
  std::array<char, 24> digits{};
  int n = 0;
  auto v = static_cast<std::uint64_t>(value < 0 ? -value : value);
  do {
    digits[static_cast<std::size_t>(n++)] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v != 0);
  if (value < 0) {
    out += '-';
  }
  for (int i = n; i < width; ++i) {
    out += '0';
  }
  while (n > 0) {
    out += digits[static_cast<std::size_t>(--n)];
  }
}

void put_date_time(std::string& out, const Civil& c, char date_sep, char between, char time_sep) {
  put_padded(out, c.year, 4);
  if (date_sep != 0) {
    out += date_sep;
  }
  put_padded(out, c.month, 2);
  if (date_sep != 0) {
    out += date_sep;
  }
  put_padded(out, c.day, 2);
  if (between != 0) {
    out += between;
  }
  put_padded(out, c.hour, 2);
  if (time_sep != 0) {
    out += time_sep;
  }
  put_padded(out, c.minute, 2);
  if (time_sep != 0) {
    out += time_sep;
  }
  put_padded(out, c.second, 2);
}

constexpr std::array<std::string_view, 7> kDays = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr std::array<std::string_view, 12> kMonths = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

bool is_digit(char c) {
  return c >= '0' && c <= '9';
}

// Reads exactly `n` digits at `pos`.
std::optional<int> digits_at(std::string_view s, std::size_t pos, std::size_t n) {
  if (pos + n > s.size()) {
    return std::nullopt;
  }
  int v = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (!is_digit(s[pos + i])) {
      return std::nullopt;
    }
    v = v * 10 + (s[pos + i] - '0');
  }
  return v;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

// Reads `.digits` (or `,digits`) as nanoseconds. `digits` has at least 1 digit.
std::int32_t fraction_nanos(std::string_view digits) {
  std::int32_t v = 0;
  std::size_t i = 0;
  for (; i < 9; ++i) {
    v = v * 10 + (i < digits.size() ? digits[i] - '0' : 0);
  }
  return v;
}

}  // namespace

std::string format_to_fs_number(Timestamp t) {
  std::string out;
  put_date_time(out, to_civil(t), 0, 0, 0);
  return out;
}

std::string format_cache_version(Timestamp t) {
  std::string out = format_to_fs_number(t);
  put_padded(out, t.micros(), 6);
  return out;
}

std::string format_iso8601(Timestamp t) {
  std::string out;
  put_date_time(out, to_civil(t), '-', 'T', ':');
  out += 'Z';
  return out;
}

std::string format_iso8601_millis(Timestamp t) {
  std::string out;
  put_date_time(out, to_civil(t), '-', 'T', ':');
  out += '.';
  put_padded(out, t.nanos / 1'000'000, 3);
  out += 'Z';
  return out;
}

std::string format_db(Timestamp t) {
  std::string out;
  put_date_time(out, to_civil(t), '-', ' ', ':');
  if (t.micros() != 0) {
    out += '.';
    put_padded(out, t.micros(), 6);
  }
  return out;
}

std::optional<Timestamp> parse_db(std::string_view text) {
  text = trim(text);
  if (text.ends_with(" UTC")) {
    text.remove_suffix(4);
  } else if (text.ends_with('Z')) {
    text.remove_suffix(1);
  }
  if (text.size() < 19 || text[4] != '-' || text[7] != '-' || (text[10] != ' ' && text[10] != 'T') || text[13] != ':' ||
      text[16] != ':') {
    return std::nullopt;
  }
  const auto year = digits_at(text, 0, 4);
  const auto month = digits_at(text, 5, 2);
  const auto day = digits_at(text, 8, 2);
  const auto hour = digits_at(text, 11, 2);
  const auto minute = digits_at(text, 14, 2);
  const auto second = digits_at(text, 17, 2);
  if (!year || !month || !day || !hour || !minute || !second) {
    return std::nullopt;
  }
  std::int32_t nanos = 0;
  std::string_view fraction = text.substr(19);
  if (!fraction.empty()) {
    if (fraction.front() != '.' || fraction.size() < 2) {
      return std::nullopt;
    }
    fraction.remove_prefix(1);
    for (const char c : fraction) {
      if (!is_digit(c)) {
        return std::nullopt;
      }
    }
    nanos = fraction_nanos(fraction.substr(0, 6));  // at most 6 digits, as the Rust port reads them
  }
  return from_civil(*year, *month, *day, *hour, *minute, *second, nanos);
}

std::string format_httpdate(Timestamp t) {
  const Civil c = to_civil(t);
  std::string out;
  out += kDays[static_cast<std::size_t>(c.weekday)];
  out += ", ";
  put_padded(out, c.day, 2);
  out += ' ';
  out += kMonths[static_cast<std::size_t>(c.month - 1)];
  out += ' ';
  put_padded(out, c.year, 4);
  out += ' ';
  put_padded(out, c.hour, 2);
  out += ':';
  put_padded(out, c.minute, 2);
  out += ':';
  put_padded(out, c.second, 2);
  out += " GMT";
  return out;
}

std::optional<Timestamp> parse_httpdate(std::string_view text) {
  std::string_view s = trim(text);
  std::optional<int> weekday;
  if (const std::size_t comma = s.find(','); comma != std::string_view::npos) {
    const std::string_view name = trim(s.substr(0, comma));
    for (std::size_t i = 0; i < kDays.size(); ++i) {
      if (name.size() == 3 && std::string_view(name) == kDays[i]) {
        weekday = static_cast<int>(i);
      }
    }
    if (!weekday) {
      return std::nullopt;
    }
    s = trim(s.substr(comma + 1));
  }
  // Split the rest on spaces: day month year time zone.
  std::array<std::string_view, 5> parts;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    s = trim(s);
    const std::size_t end = i + 1 == parts.size() ? s.size() : s.find(' ');
    if (end == std::string_view::npos || end == 0) {
      return std::nullopt;
    }
    parts[i] = s.substr(0, end);
    s.remove_prefix(end);
  }
  if (!trim(s).empty()) {
    return std::nullopt;
  }
  // Day.
  if (parts[0].size() > 2) {
    return std::nullopt;
  }
  int day = 0;
  for (const char c : parts[0]) {
    if (!is_digit(c)) {
      return std::nullopt;
    }
    day = day * 10 + (c - '0');
  }
  // Month.
  int month = 0;
  for (std::size_t i = 0; i < kMonths.size(); ++i) {
    if (parts[1] == kMonths[i]) {
      month = static_cast<int>(i) + 1;
    }
  }
  if (month == 0) {
    return std::nullopt;
  }
  // Year: RFC 2822 gives 2 digits (00 to 49 mean 20xx, 50 to 99 mean 19xx) and 3 digits (+1900).
  int year = 0;
  if (parts[2].size() < 2 || parts[2].size() > 4) {
    return std::nullopt;
  }
  for (const char c : parts[2]) {
    if (!is_digit(c)) {
      return std::nullopt;
    }
    year = year * 10 + (c - '0');
  }
  if (parts[2].size() == 2) {
    year += year < 50 ? 2000 : 1900;
  } else if (parts[2].size() == 3) {
    year += 1900;
  }
  // Time: hh:mm[:ss].
  const std::string_view clock = parts[3];
  const auto hour = digits_at(clock, 0, 2);
  const auto minute = clock.size() >= 5 && clock[2] == ':' ? digits_at(clock, 3, 2) : std::nullopt;
  std::optional<int> second = 0;
  if (clock.size() == 8 && clock[5] == ':') {
    second = digits_at(clock, 6, 2);
  } else if (clock.size() != 5) {
    return std::nullopt;
  }
  if (!hour || !minute || !second) {
    return std::nullopt;
  }
  // Zone, in minutes east of UTC.
  const std::string_view zone = parts[4];
  int offset = 0;
  struct NamedZone {
    std::string_view name;
    int minutes;
  };
  static constexpr std::array<NamedZone, 12> kZones = {{{"GMT", 0},
                                                        {"UT", 0},
                                                        {"UTC", 0},
                                                        {"Z", 0},
                                                        {"EST", -5 * 60},
                                                        {"EDT", -4 * 60},
                                                        {"CST", -6 * 60},
                                                        {"CDT", -5 * 60},
                                                        {"MST", -7 * 60},
                                                        {"MDT", -6 * 60},
                                                        {"PST", -8 * 60},
                                                        {"PDT", -7 * 60}}};
  const auto named = std::ranges::find(kZones, zone, &NamedZone::name);
  if (named != kZones.end()) {
    offset = named->minutes;
  } else if (zone.size() == 5 && (zone[0] == '+' || zone[0] == '-')) {
    const auto hh = digits_at(zone, 1, 2);
    const auto mm = digits_at(zone, 3, 2);
    if (!hh || !mm || *mm > 59) {
      return std::nullopt;
    }
    offset = (*hh * 60 + *mm) * (zone[0] == '-' ? -1 : 1);
  } else {
    return std::nullopt;
  }
  const auto local = from_civil(year, month, day, *hour, *minute, *second == 60 ? 59 : *second);
  if (!local) {
    return std::nullopt;
  }
  if (weekday && to_civil(*local).weekday != *weekday) {
    return std::nullopt;
  }
  return local->plus_seconds(-static_cast<std::int64_t>(offset) * 60);
}

std::int64_t epoch_ms(Timestamp t) {
  // `Time#to_f` is the double nearest to the exact value. Parsing the decimal text gives that.
  std::int64_t seconds = t.seconds;
  std::int64_t nanos = t.nanos;
  const char* sign = "";
  if (seconds < 0 && nanos != 0) {
    seconds += 1;
    nanos = 1'000'000'000 - nanos;
    if (seconds == 0) {
      sign = "-";
    }
  }
  std::array<char, 48> text{};
  std::snprintf(text.data(), text.size(), "%s%lld.%09lld", sign, static_cast<long long>(seconds),
                static_cast<long long>(nanos));
  const double to_f = std::strtod(text.data(), nullptr);
  return static_cast<std::int64_t>(to_f * 1000.0);
}

Result<Timestamp> parse_rfc3339(std::string_view text) {
  const auto bad = [&](std::string_view why) { return fail(Errc::Parse, std::string(why)); };
  const std::string_view s = trim(text);
  if (s.size() < 11) {
    return bad("too short");
  }
  const auto year = digits_at(s, 0, 4);
  const auto month = digits_at(s, 5, 2);
  const auto day = digits_at(s, 8, 2);
  if (!year || !month || !day || s[4] != '-' || s[7] != '-') {
    return bad("invalid date");
  }
  if (s[10] != 'T' && s[10] != 't' && s[10] != ' ') {
    return bad("expected a T between the date and the time");
  }
  std::size_t pos = 11;
  const auto hour = digits_at(s, pos, 2);
  if (!hour) {
    return bad("invalid hour");
  }
  pos += 2;
  int minute = 0;
  int second = 0;
  std::int32_t nanos = 0;
  if (pos < s.size() && s[pos] == ':') {
    const auto m = digits_at(s, pos + 1, 2);
    if (!m) {
      return bad("invalid minute");
    }
    minute = *m;
    pos += 3;
    if (pos < s.size() && s[pos] == ':') {
      const auto sec = digits_at(s, pos + 1, 2);
      if (!sec) {
        return bad("invalid second");
      }
      second = *sec;
      pos += 3;
      if (pos < s.size() && (s[pos] == '.' || s[pos] == ',')) {
        std::size_t end = pos + 1;
        while (end < s.size() && is_digit(s[end])) {
          ++end;
        }
        if (end == pos + 1 || end - pos - 1 > 9) {
          return bad("invalid fraction of a second");
        }
        nanos = fraction_nanos(s.substr(pos + 1, end - pos - 1));
        pos = end;
      }
    }
  }
  if (second == 60) {
    second = 59;
  }
  if (pos >= s.size()) {
    return bad("missing offset");
  }
  std::int64_t offset = 0;  // seconds east of UTC
  if (s[pos] == 'Z' || s[pos] == 'z') {
    ++pos;
  } else if (s[pos] == '+' || s[pos] == '-') {
    const int sign = s[pos] == '-' ? -1 : 1;
    const auto oh = digits_at(s, pos + 1, 2);
    if (!oh) {
      return bad("invalid offset");
    }
    pos += 3;
    int om = 0;
    int os = 0;
    if (pos < s.size() && s[pos] == ':') {
      ++pos;
    }
    if (pos < s.size()) {
      const auto m = digits_at(s, pos, 2);
      if (!m) {
        return bad("invalid offset");
      }
      om = *m;
      pos += 2;
      if (pos < s.size() && s[pos] == ':') {
        ++pos;
      }
      if (pos < s.size()) {
        const auto sec = digits_at(s, pos, 2);
        if (!sec) {
          return bad("invalid offset");
        }
        os = *sec;
        pos += 2;
      }
    }
    if (*oh > 24 || om > 59 || os > 59) {
      return bad("offset out of range");
    }
    offset = static_cast<std::int64_t>(sign) * (*oh * 3600 + om * 60 + os);
  } else {
    return bad("missing offset");
  }
  if (pos != s.size()) {
    return bad("trailing characters");
  }
  const auto local = from_civil(*year, *month, *day, *hour, minute, second, nanos);
  if (!local) {
    return bad("date or time out of range");
  }
  const Timestamp result = local->plus_seconds(-offset);
  if (result.seconds < kMinSeconds || result.seconds > kMaxSeconds) {
    return bad("out of range");
  }
  return result;
}

Timestamp years_from(Timestamp t, int years) {
  const Civil c = to_civil(t);
  const int year = c.year + years;
  if (year < 1 || year > 9999) {
    return t;
  }
  const int day = c.day > days_in_month(year, c.month) ? days_in_month(year, c.month) : c.day;
  const auto result = from_civil(year, c.month, day, c.hour, c.minute, c.second, t.nanos);
  return result.value_or(t);
}

}  // namespace campfire
