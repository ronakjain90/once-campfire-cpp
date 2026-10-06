// UsersHelper and string helpers that the room views use. Rust: crates/views/src/helpers/users.rs.
#include "views/helpers/users.hpp"

#include <cctype>

namespace campfire::views::helpers {

namespace {

bool is_word_byte(unsigned char c) {
  return std::isalnum(c) != 0 || c == '_' || c >= 0x80;
}

}  // namespace

std::string capitalize(std::string_view text) {
  std::string out(text);
  for (std::size_t i = 0; i < out.size(); ++i) {
    const auto c = static_cast<unsigned char>(out[i]);
    if (c >= 0x80) continue;
    out[i] = static_cast<char>(i == 0 ? std::toupper(c) : std::tolower(c));
  }
  return out;
}

std::string to_sentence(const std::vector<std::string>& items, std::string_view two_words_connector) {
  std::string out;
  switch (items.size()) {
    case 0:
      return out;
    case 1:
      return items[0];
    case 2:
      return items[0] + std::string(two_words_connector) + items[1];
    default:
      break;
  }
  for (std::size_t i = 0; i + 1 < items.size(); ++i) {
    if (i > 0) out += ", ";
    out += items[i];
  }
  out += ", and ";
  out += items.back();
  return out;
}

std::string first_character(std::string_view text) {
  if (text.empty()) return {};
  std::size_t n = 1;
  while (n < text.size() && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) ++n;
  return std::string(text.substr(0, n));
}

std::string initials(std::string_view name) {
  // `name.scan(/\b[[:alnum:]]/)`: an ASCII letter or digit that does not follow a word character.
  std::string out;
  bool previous_word = false;
  for (const char ch : name) {
    const auto c = static_cast<unsigned char>(ch);
    if ((std::isalnum(c) != 0 || c == '_') && !previous_word) out.push_back(ch);
    previous_word = is_word_byte(c);
  }
  return out;
}

Attrs sidebar_turbo_frame_options(std::optional<std::string_view> src) {
  Attrs options;
  options.set("data-turbo-permanent", Value(true));
  options.set("data-controller", Value("rooms-list read-rooms turbo-frame"));
  options.set("data-rooms-list-unread-class", Value("unread"));
  // `html_safe` in the reference, so that "->" is not escaped.
  options.set("data-action",
              Value(SafeHtml::trusted("presence:present@window->rooms-list#read read-rooms:read->rooms-list#read "
                                      "turbo:frame-load->rooms-list#loaded refresh-room:visible@window->turbo-frame#reload")));
  options.set("id", Value("user_sidebar"));
  if (src) {
    options.set("src", Value(*src));
  } else {
    options.set("src", std::nullopt);
  }
  options.set("target", Value("_top"));
  return options;
}

void sidebar_turbo_frame_tag(Out& out, std::optional<std::string_view> src, SafeHtml content) {
  content_tag(out, "turbo-frame", sidebar_turbo_frame_options(src), [&](Out& body) { body.append(content); });
}

}  // namespace campfire::views::helpers
