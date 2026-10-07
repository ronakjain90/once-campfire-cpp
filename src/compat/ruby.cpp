// Ruby's string behavior (see ruby.hpp). Rust: crates/ruby/src/{erb,integer,string,uri,rack}.rs.
#include "compat/ruby.hpp"

#include <algorithm>

namespace campfire::compat {
namespace {

__extension__ typedef __int128 i128;

// Ruby's ISSPACE: what String#to_i and #to_f skip.
bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

std::string_view trim_start_space(std::string_view s) {
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  return s;
}

constexpr i128 kI128Max = i128(~(unsigned __int128)0 >> 1);

// String#to_i: optional leading whitespace and sign, an optional "0d", then digits (an
// underscore allowed between two). Saturating at the i128 bounds.
i128 to_i128(std::string_view s) {
  s = trim_start_space(s);
  bool negative = false;
  if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
    negative = s[0] == '-';
    s.remove_prefix(1);
  }
  if (s.size() >= 2 && s[0] == '0' && (s[1] == 'd' || s[1] == 'D')) s.remove_prefix(2);
  i128 number = 0;
  bool previous_digit = false;
  for (char c : s) {
    if (c >= '0' && c <= '9') {
      if (number > (kI128Max - (c - '0')) / 10)
        number = kI128Max;
      else
        number = (number << 3) + (number << 1) + (c - '0');  // no i128 multiply: UBSan needs __muloti4
      previous_digit = true;
    } else if (c == '_' && previous_digit) {
      previous_digit = false;
    } else {
      break;
    }
  }
  return negative ? -number : number;
}

}  // namespace

// ---- ERB ------------------------------------------------------------------------------------

void append_html_escaped(std::string& out, std::string_view s) {
  size_t last = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    const char* replacement;
    switch (s[i]) {
      case '&': replacement = "&amp;"; break;
      case '<': replacement = "&lt;"; break;
      case '>': replacement = "&gt;"; break;
      case '"': replacement = "&quot;"; break;
      case '\'': replacement = "&#39;"; break;
      default: continue;
    }
    out.append(s.data() + last, i - last);
    out += replacement;
    last = i + 1;
  }
  out.append(s.data() + last, s.size() - last);
}

std::string html_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + s.size() / 8);
  append_html_escaped(out, s);
  return out;
}

// ---- strings and integers -------------------------------------------------------------------

std::string_view strip(std::string_view s) {
  auto strippable = [](char c) { return c == '\0' || is_space(c); };
  while (!s.empty() && strippable(s.front())) s.remove_prefix(1);
  while (!s.empty() && strippable(s.back())) s.remove_suffix(1);
  return s;
}

int64_t to_i(std::string_view s) {
  i128 n = to_i128(s);
  return int64_t(std::clamp<i128>(n, INT64_MIN, INT64_MAX));
}

std::optional<int64_t> to_i_checked(std::string_view s) {
  i128 n = to_i128(s);
  if (n < INT64_MIN || n > INT64_MAX) return std::nullopt;
  return int64_t(n);
}

std::optional<int64_t> integer_cast(std::string_view s) {
  std::string_view unsigned_part = trim_start_space(s);
  if (!unsigned_part.empty() && (unsigned_part[0] == '+' || unsigned_part[0] == '-')) unsigned_part.remove_prefix(1);
  if (unsigned_part.empty() || unsigned_part[0] < '0' || unsigned_part[0] > '9') return std::nullopt;
  return to_i_checked(s);
}

// ---- URI escaping ---------------------------------------------------------------------------

namespace {

std::string percent_encode(std::string_view s, bool space_as_plus) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() + s.size() / 2);
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
        c == '-' || c == '~') {
      out += char(c);
    } else if (c == ' ' && space_as_plus) {
      out += '+';
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

}  // namespace

std::string cgi_escape(std::string_view s) {
  return percent_encode(s, true);
}
std::string url_encode(std::string_view s) {
  return percent_encode(s, false);
}

// ---- Rack byte ranges -----------------------------------------------------------------------

namespace {

// `http_range =~ /bytes=([^;]+)/`: after the first "bytes=" that something other than ";"
// follows, up to the next ";".
std::optional<std::string_view> range_spec(std::string_view header) {
  size_t from = 0;
  while (true) {
    size_t i = header.find("bytes=", from);
    if (i == std::string_view::npos) return std::nullopt;
    std::string_view rest = header.substr(i + 6);
    size_t semi = rest.find(';');
    std::string_view spec = rest.substr(0, semi);
    if (!spec.empty()) return spec;
    from = i + 1;
  }
}

// String#split with a separator finder: trailing empty fields are dropped.
template <typename Find>
std::vector<std::string_view> ruby_split(std::string_view s, Find find) {
  std::vector<std::string_view> fields;
  std::string_view rest = s;
  while (auto m = find(rest)) {
    fields.push_back(rest.substr(0, m->first));
    rest = rest.substr(m->second);
  }
  fields.push_back(rest);
  while (!fields.empty() && fields.back().empty()) fields.pop_back();
  return fields;
}

}  // namespace

std::optional<std::vector<ByteRange>> byte_ranges(std::optional<std::string_view> header, uint64_t size_u) {
  if (size_u == 0 || !header) return std::nullopt;
  auto spec_opt = range_spec(*header);
  if (!spec_opt) return std::nullopt;
  std::string_view spec = *spec_opt;
  if (std::count(spec.begin(), spec.end(), ',') >= 100) return std::nullopt;
  i128 size = size_u;
  std::vector<std::pair<i128, i128>> ranges;
  auto split_comma = [](std::string_view s) -> std::optional<std::pair<size_t, size_t>> {
    size_t i = s.find(',');
    if (i == std::string_view::npos) return std::nullopt;
    size_t end = i + 1;
    while (end < s.size() && (s[end] == ' ' || s[end] == '\t')) ++end;
    return std::pair{i, end};
  };
  auto split_dash = [](std::string_view s) -> std::optional<std::pair<size_t, size_t>> {
    size_t i = s.find('-');
    if (i == std::string_view::npos) return std::nullopt;
    return std::pair{i, i + 1};
  };
  for (std::string_view one : ruby_split(spec, split_comma)) {
    if (one.find('-') == std::string_view::npos) return std::nullopt;
    // Split on "-" first, so neither end can be negative.
    auto parts = ruby_split(one, split_dash);
    std::optional<std::string_view> r0, r1;
    if (!parts.empty()) r0 = parts[0];
    if (parts.size() > 1) r1 = parts[1];
    i128 first, last;
    if (!r0 || r0->empty()) {
      if (!r1) return std::nullopt;
      first = std::max<i128>(size - to_i128(*r1), 0);
      last = size - 1;
    } else {
      first = to_i128(*r0);
      if (!r1) {
        last = size - 1;
      } else {
        i128 end = to_i128(*r1);
        if (end < first) return std::nullopt;
        last = std::min<i128>(end, size - 1);
      }
    }
    if (first <= last) ranges.emplace_back(first, last);
  }
  i128 total = 0;
  for (auto& [a, b] : ranges) total += b - a + 1;
  if (total > size) return std::vector<ByteRange>{};
  std::vector<ByteRange> out;
  out.reserve(ranges.size());
  for (auto& [a, b] : ranges) out.push_back({uint64_t(a), uint64_t(b)});
  return out;
}

}  // namespace campfire::compat
