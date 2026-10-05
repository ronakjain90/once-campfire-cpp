// See disposition.hpp.
#include "storage/disposition.hpp"

#include <cctype>

#include "compat/content_disposition.hpp"

namespace campfire::storage {

namespace {

std::string percent_escape(std::string_view s, std::string_view extra) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    auto u = static_cast<unsigned char>(c);
    if ((u < 0x80 && std::isalnum(u)) || (u < 0x80 && extra.find(c) != std::string_view::npos)) {
      out.push_back(c);
    } else {
      out.push_back('%');
      out.push_back(kHex[u >> 4]);
      out.push_back(kHex[u & 15]);
    }
  }
  return out;
}

}  // namespace

std::string content_disposition_with(std::string_view disposition, std::string_view sanitized_filename) {
  return compat::content_disposition(disposition == "attachment" ? "attachment" : "inline", sanitized_filename);
}

std::string escape_path(std::string_view s) { return percent_escape(s, "-._~!$&'()*+,;=:@/"); }
std::string escape_segment(std::string_view s) { return percent_escape(s, "-._~!$&'()*+,;=:@"); }

}  // namespace campfire::storage
