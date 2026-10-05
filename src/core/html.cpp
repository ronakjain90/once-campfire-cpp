// Matches ERB::Util.html_escape (ActiveSupport core_ext/string/output_safety.rb).
#include "core/html.hpp"

#include <array>

#include "core/out.hpp"

namespace campfire {
namespace {

// The replacement of a byte, or an empty view when the byte stays.
constexpr std::string_view replacement(char c) noexcept {
  switch (c) {
    case '&': return "&amp;";
    case '<': return "&lt;";
    case '>': return "&gt;";
    case '"': return "&quot;";
    case '\'': return "&#39;";
    default: return {};
  }
}

constexpr std::array<bool, 256> make_table() {
  std::array<bool, 256> table{};
  for (const char c : std::string_view("&<>\"'")) {
    table[static_cast<unsigned char>(c)] = true;
  }
  return table;
}
constexpr std::array<bool, 256> kSpecial = make_table();

}  // namespace

bool needs_html_escape(std::string_view text) noexcept {
  for (const char c : text) {
    if (kSpecial[static_cast<unsigned char>(c)]) {
      return true;
    }
  }
  return false;
}

void html_escape(Out& out, std::string_view text) {
  std::size_t run_start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (!kSpecial[static_cast<unsigned char>(text[i])]) {
      continue;
    }
    out.append_raw(text.substr(run_start, i - run_start));
    out.append_raw(replacement(text[i]));
    run_start = i + 1;
  }
  out.append_raw(text.substr(run_start));
}

}  // namespace campfire
