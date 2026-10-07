// The search query. Rails: app/controllers/searches_controller.rb (`query`). Rust: crates/campfire/src/integrations/
// search.rs, crates/db/src/models/message.rs (`match_terms`).
#include "models/search_query.hpp"

#include <cstddef>
#include <cstdint>

namespace campfire::models::search_query {

namespace {

struct Decoded {
  char32_t code = 0;
  std::size_t size = 0;  // 0: the bytes are not valid UTF-8
};

bool continuation(std::string_view s, std::size_t i) {
  return i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0U) == 0x80U;
}

Decoded decode(std::string_view s, std::size_t i) {
  const auto b0 = static_cast<unsigned char>(s[i]);
  if (b0 < 0x80U) return {b0, 1};
  const auto cont = [&](std::size_t k) { return static_cast<char32_t>(static_cast<unsigned char>(s[i + k]) & 0x3FU); };
  if (b0 >= 0xC2U && b0 <= 0xDFU && continuation(s, i + 1)) return {((b0 & 0x1FU) << 6U) | cont(1), 2};
  if (b0 >= 0xE0U && b0 <= 0xEFU && continuation(s, i + 1) && continuation(s, i + 2)) {
    const char32_t c = ((b0 & 0x0FU) << 12U) | (cont(1) << 6U) | cont(2);
    if (c >= 0x800U && (c < 0xD800U || c > 0xDFFFU)) return {c, 3};
  }
  if (b0 >= 0xF0U && b0 <= 0xF4U && continuation(s, i + 1) && continuation(s, i + 2) && continuation(s, i + 3)) {
    const char32_t c = ((b0 & 0x07U) << 18U) | (cont(1) << 12U) | (cont(2) << 6U) | cont(3);
    if (c >= 0x10000U && c <= 0x10FFFFU) return {c, 4};
  }
  return {};
}

// Ruby `/[[:space:]]/` for the characters that `String#blank?` treats as white space.
bool is_space(char32_t c) noexcept {
  return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

}  // namespace

std::string sanitize(std::string_view q) {
  std::string out;
  out.reserve(q.size());
  std::size_t i = 0;
  while (i < q.size()) {
    const Decoded d = decode(q, i);
    if (d.size == 0) {
      out.push_back(' ');
      ++i;
    } else if (is_word(d.code)) {
      out.append(q.substr(i, d.size));
      i += d.size;
    } else {
      out.push_back(' ');
      i += d.size;
    }
  }
  return out;
}

bool is_present(std::string_view query) noexcept {
  std::size_t i = 0;
  while (i < query.size()) {
    const Decoded d = decode(query, i);
    if (d.size == 0 || !is_space(d.code)) return true;
    i += d.size;
  }
  return false;
}

std::string match_terms(std::string_view query) {
  std::string out;
  std::size_t i = 0;
  while (i < query.size()) {
    const Decoded d = decode(query, i);
    if (query[i] == '\0' || (d.size != 0 && is_space(d.code))) {
      ++i;
      continue;
    }
    // A word runs to the next white space or NUL.
    std::size_t end = i;
    while (end < query.size()) {
      const Decoded e = decode(query, end);
      if (query[end] == '\0' || (e.size != 0 && is_space(e.code))) break;
      end += e.size == 0 ? 1 : e.size;
    }
    if (!out.empty()) out.push_back(' ');
    out.push_back('"');
    for (const char c : query.substr(i, end - i)) {
      if (c == '"') out.push_back('"');
      out.push_back(c);
    }
    out.push_back('"');
    i = end;
  }
  return out;
}

}  // namespace campfire::models::search_query
