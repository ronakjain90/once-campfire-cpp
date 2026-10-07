// String#to_f and Float#to_s as Ruby 3.4 does them. Rust: crates/ruby/src/float.rs.
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "compat/float_digits.hpp"
#include "compat/ruby.hpp"

namespace campfire::compat {
namespace {

using Bytes = std::string_view;

bool is_space(unsigned char c) {
  return c == ' ' || (c >= '\t' && c <= '\r');
}
bool is_digit10(unsigned char c) {
  return c >= '0' && c <= '9';
}
int digit_value(unsigned char c, int base) {
  int v = -1;
  if (c >= '0' && c <= '9')
    v = c - '0';
  else if (c >= 'a' && c <= 'f')
    v = c - 'a' + 10;
  else if (c >= 'A' && c <= 'F')
    v = c - 'A' + 10;
  return v >= 0 && v < base ? v : -1;
}
bool is_digit(unsigned char c, int base) {
  return digit_value(c, base) >= 0;
}
unsigned char lower(unsigned char c) {
  return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

unsigned char at(Bytes s, size_t i) {
  return i < s.size() ? static_cast<unsigned char>(s[i]) : 0;
}

bool is_hex(Bytes s) {
  return at(s, 0) == '0' && (at(s, 1) == 'x' || at(s, 1) == 'X');
}

constexpr size_t kSignificandWidth = 60;
constexpr size_t kBufferWidth = 69;
constexpr size_t kFractionDigits = 60;

// The number copied into a fixed buffer with the underscores between digits left out
// (rb_cstr_to_dbl's second strtod input).
std::string without_underscores(Bytes s, size_t end) {
  std::string out;
  size_t width = kSignificandWidth;
  int base = 10;
  unsigned char exponent_letter = 'e';
  bool dot_seen = false;
  unsigned char previous = 0;
  size_t p = 0;
  if (at(s, p) == '+' || at(s, p) == '-') {
    previous = at(s, p);
    out += char(previous);
    ++p;
  }
  if (at(s, p) == '0') {
    previous = '0';
    out += '0';
    ++p;
    if (at(s, p) == 'x' || at(s, p) == 'X') {
      previous = 'x';
      out += 'x';
      base = 16;
      exponent_letter = 'p';
      ++p;
    }
    while (at(s, p) == '0') ++p;
  }
  while (p < end && out.size() < width) {
    previous = static_cast<unsigned char>(s[p]);
    out += char(previous);
    ++p;
  }
  while (p < s.size()) {
    if (s[p] == '_') {
      ++p;
      if (out.empty() || !is_digit(previous, base) || !is_digit(at(s, p), base)) break;
    }
    previous = static_cast<unsigned char>(s[p]);
    ++p;
    if (width == kSignificandWidth && lower(previous) == exponent_letter) {
      width = kBufferWidth;
      out += char(previous);
      if (at(s, p) == '+' || at(s, p) == '-') {
        previous = at(s, p);
        out += char(previous);
        ++p;
      }
      if (at(s, p) == '0') {
        previous = '0';
        out += '0';
        while (at(s, p) == '0') ++p;
      }
      base = 10;
      continue;
    } else if (is_space(previous)) {
      while (is_space(at(s, p))) ++p;
      if (p < s.size()) break;
    } else if (previous == '.') {
      bool was = dot_seen;
      dot_seen = true;
      if (was) break;
    } else if (!is_digit(previous, base)) {
      break;
    }
    if (out.size() < width) out += char(previous);
  }
  return out;
}

struct Parsed {
  double value;
  size_t end;
};

// Exponent at s[e] (the 'e'), and where it ends; e itself when no digit follows. Past 19999 it is 19999.
std::pair<int, size_t> strtod_exponent(Bytes s, size_t e) {
  size_t i = e + 1;
  bool negative = at(s, i) == '-';
  if (at(s, i) == '+' || at(s, i) == '-') ++i;
  if (!is_digit10(at(s, i))) return {0, e};
  while (at(s, i) == '0') ++i;
  size_t start = i;
  int64_t exponent = 0;
  while (is_digit10(at(s, i))) {
    exponent = std::min<int64_t>(exponent * 10 + (at(s, i) - '0'), int64_t(1) << 40);
    ++i;
  }
  int e32 = (i - start > 8 || exponent > 19999) ? 19999 : int(exponent);
  return {negative ? -e32 : e32, i};
}

Parsed hex_strtod(Bytes s, size_t start, bool negative) {
  auto hex = [&](size_t i) { return digit_value(at(s, i), 16); };
  auto signed_ = [&](double v) { return negative ? -v : v; };
  size_t i = start;
  if (hex(i) < 0 && at(s, i) != '.') return {0.0, 0};
  double sum = 0, weight = 1;
  int64_t exponent = -4;
  while (at(s, i) == '0') ++i;
  if (i == s.size()) return {signed_(0.0), i};
  while (hex(i) >= 0) {
    sum += weight * hex(i);
    exponent += 4;
    weight /= 16;
    ++i;
  }
  if (at(s, i) == '.') {
    ++i;
    if (hex(i) >= 0) {
      if (exponent < 0) {
        while (at(s, i) == '0') {
          ++i;
          exponent -= 4;
        }
      }
      while (hex(i) >= 0) {
        sum += weight * hex(i);
        ++i;
        weight /= 16;
        if (weight == 0.0) {
          while (hex(i) >= 0) ++i;
          break;
        }
      }
    }
  }
  if (at(s, i) == 'p' || at(s, i) == 'P') {
    ++i;
    int sign = 0;
    if (at(s, i) == '-')
      sign = -1;
    else if (at(s, i) == '+')
      sign = 1;
    if (sign != 0) ++i;
    if (sign == 0) sign = 1;
    if (!is_digit10(at(s, i))) return {0.0, 0};
    int64_t power = 0;
    while (is_digit10(at(s, i))) {
      power = power * 10 + (at(s, i) - '0');
      ++i;
      if (power + sign * exponent > 2095) {
        while (is_digit10(at(s, i))) ++i;
        break;
      }
    }
    exponent += power * sign;
  }
  int e = int(std::clamp<int64_t>(exponent, -5000, 5000));
  return {signed_(std::ldexp(sum, e)), i};
}

// ruby_strtod (David Gay's, in Ruby's missing/dtoa.c): the number at the start of s and how
// much of s it takes (0 for none).
Parsed strtod(Bytes s) {
  size_t i = 0;
  while (is_space(at(s, i))) ++i;
  bool negative = at(s, i) == '-';
  if (at(s, i) == '+' || at(s, i) == '-') ++i;
  if (i == s.size()) return {0.0, 0};
  if (at(s, i) == '0' && (at(s, i + 1) == 'x' || at(s, i + 1) == 'X')) return hex_strtod(s, i + 2, negative);
  double signed_zero = negative ? -0.0 : 0.0;

  bool leading_zero = at(s, i) == '0';
  while (at(s, i) == '0') ++i;
  if (leading_zero && i == s.size()) return {signed_zero, i};
  size_t integer_start = i;
  while (is_digit10(at(s, i))) ++i;
  Bytes integer = s.substr(integer_start, i - integer_start);
  size_t digits = integer.size();
  std::string fraction;
  bool fraction_zeros = false;
  if (at(s, i) == '.' && is_digit10(at(s, i + 1))) {
    ++i;
    size_t zeros = 0;
    while (is_digit10(at(s, i))) {
      unsigned char digit = at(s, i);
      ++i;
      if (digits > kFractionDigits) continue;
      if (digit == '0') {
        ++zeros;
        fraction_zeros = true;
        continue;
      }
      fraction.append(zeros, '0');
      fraction += char(digit);
      digits += digits == 0 ? 1 : zeros + 1;
      zeros = 0;
    }
  } else if (at(s, i) == '.') {
    ++i;
  }
  bool any_digits = digits > 0 || fraction_zeros || leading_zero;

  int exponent = 0;
  if (at(s, i) == 'e' || at(s, i) == 'E') {
    if (!any_digits) return {0.0, 0};
    auto [e, end] = strtod_exponent(s, i);
    exponent = e;
    i = end;
  }
  if (digits == 0) return any_digits ? Parsed{signed_zero, i} : Parsed{0.0, 0};

  std::string number;
  if (negative) number += '-';
  number.append(integer);
  number += '.';
  number += fraction;
  number += 'e';
  number += std::to_string(exponent);
  double value;
  auto r = std::from_chars(number.data(), number.data() + number.size(), value);
  if (r.ec == std::errc::result_out_of_range) {
    // Too large is infinity and too small is zero. The decimal order says which.
    int64_t order;
    if (!integer.empty()) {
      order = int64_t(integer.size()) - 1 + exponent;
    } else {
      size_t z = 0;
      while (z < fraction.size() && fraction[z] == '0') ++z;
      order = -int64_t(z) - 1 + exponent;
    }
    value = order > 0 ? std::numeric_limits<double>::infinity() : 0.0;
    if (negative) value = -value;
  } else if (r.ec != std::errc()) {
    value = signed_zero;
  }
  return {value, i};
}

}  // namespace

double to_f(std::string_view input) {
  size_t nul = input.find('\0');
  Bytes s = input.substr(0, nul);
  while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  if (is_hex(s)) return 0.0;
  Parsed first = strtod(s);
  if (first.end == 0 || first.end == s.size()) return first.value;
  std::string number = without_underscores(s, first.end);
  if (is_hex(number)) return 0.0;
  return strtod(number).value;
}

namespace detail {

Digits shortest_digits(double magnitude) {
  auto scientific = [](char* buf, size_t cap, double v, int precision) -> std::string {
    std::to_chars_result r = precision < 0 ? std::to_chars(buf, buf + cap, v, std::chars_format::scientific)
                                           : std::to_chars(buf, buf + cap, v, std::chars_format::scientific, precision);
    return std::string(buf, r.ptr);
  };
  auto split = [](const std::string& text) -> Digits {
    size_t e = text.find('e');
    Digits d;
    for (size_t i = 0; i < e; ++i) {
      if (text[i] >= '0' && text[i] <= '9') d.digits += text[i];
    }
    d.decpt = std::stoi(text.substr(e + 1)) + 1;
    return d;
  };
  char buf[64];
  Digits shortest = split(scientific(buf, sizeof buf, magnitude, -1));
  return shortest;
}

Digits ruby_shortest_digits(double magnitude) {
  Digits shortest = shortest_digits(magnitude);
  const std::string& d = shortest.digits;
  if (d.size() >= 16 && (d.back() == '1' || d.back() == '3' || d.back() == '5' || d.back() == '7' || d.back() == '9')) {
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, magnitude, std::chars_format::scientific, int(d.size()) - 1);
    std::string even(buf, r.ptr);
    double back;
    std::from_chars(even.data(), even.data() + even.size(), back);
    if (back == magnitude) {
      size_t e = even.find('e');
      Digits out;
      for (size_t i = 0; i < e; ++i) {
        if (even[i] >= '0' && even[i] <= '9') out.digits += even[i];
      }
      out.decpt = std::stoi(even.substr(e + 1)) + 1;
      return out;
    }
  }
  return shortest;
}

}  // namespace detail

std::string float_to_s(double f) {
  if (std::isnan(f)) return "NaN";
  if (std::isinf(f)) return f > 0 ? "Infinity" : "-Infinity";
  if (f == 0.0) return std::signbit(f) ? "-0.0" : "0.0";
  // Ruby's dtoa takes the even digit when two shortest forms are equally close. See the
  // tie handling in ruby_shortest_digits.
  detail::Digits d = detail::ruby_shortest_digits(std::fabs(f));
  const std::string& digits = d.digits;
  int decpt = d.decpt;
  std::string sign = f < 0 ? "-" : "";
  if (decpt < -3 || (decpt > 15 && int(digits.size()) <= decpt)) {
    std::string rest = digits.size() > 1 ? digits.substr(1) : "0";
    int e = decpt - 1;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%c%02d", e < 0 ? '-' : '+', std::abs(e));
    return sign + digits.substr(0, 1) + "." + rest + "e" + buf;
  }
  if (decpt <= 0) return sign + "0." + std::string(size_t(-decpt), '0') + digits;
  if (size_t(decpt) >= digits.size()) return sign + digits + std::string(size_t(decpt) - digits.size(), '0') + ".0";
  return sign + digits.substr(0, size_t(decpt)) + "." + digits.substr(size_t(decpt));
}

}  // namespace campfire::compat
