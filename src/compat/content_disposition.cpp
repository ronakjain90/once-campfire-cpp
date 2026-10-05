// Content-Disposition (see content_disposition.hpp).
#include "compat/content_disposition.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace campfire::compat {
namespace {

struct Approximation {
  char32_t from;
  const char* to;
};

// I18n::Backend::Transliterator::HashTransliterator::DEFAULT_APPROXIMATIONS (i18n 1.14.7), sorted.
constexpr Approximation kApproximations[] = {
#include "compat/content_disposition_table.inc"
};

bool traditional(unsigned char c) {
  return c == ' ' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
         std::string_view("!#$+.^_`|~-").find(char(c)) != std::string_view::npos;
}

bool rfc_5987(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
         std::string_view("!#$&+.^_`|~-").find(char(c)) != std::string_view::npos;
}

std::string percent_escape(std::string_view s, bool (*keep)(unsigned char)) {
  std::string out;
  for (unsigned char c : s) {
    if (keep(c)) {
      out += char(c);
    } else {
      char buf[4];
      std::snprintf(buf, sizeof buf, "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

// Decodes one UTF-8 character. Invalid bytes give U+FFFD and advance one byte.
char32_t decode(std::string_view s, size_t& i) {
  unsigned char c = static_cast<unsigned char>(s[i]);
  size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
  if (n == 0 || i + n > s.size()) {
    ++i;
    return 0xFFFD;
  }
  char32_t cp = n == 1 ? c : c & (0xFF >> (n + 1));
  for (size_t k = 1; k < n; ++k) {
    unsigned char cc = static_cast<unsigned char>(s[i + k]);
    if ((cc & 0xC0) != 0x80) {
      ++i;
      return 0xFFFD;
    }
    cp = (cp << 6) | (cc & 0x3F);
  }
  i += n;
  return cp;
}

// I18n.transliterate with the default approximations, and "?" for anything else non-ASCII.
std::string transliterate(std::string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size();) {
    char32_t cp = decode(s, i);
    if (cp < 0x80) {
      out += char(cp);
      continue;
    }
    auto* end = std::end(kApproximations);
    auto* it = std::lower_bound(std::begin(kApproximations), end, cp,
                                [](const Approximation& a, char32_t v) { return a.from < v; });
    out += (it != end && it->from == cp) ? it->to : "?";
  }
  return out;
}

}  // namespace

std::string content_disposition(std::string_view disposition, std::string_view filename) {
  std::string out(disposition);
  out += "; filename=\"" + percent_escape(transliterate(filename), traditional) + "\"; filename*=UTF-8''" +
         percent_escape(filename, rfc_5987);
  return out;
}

}  // namespace campfire::compat
