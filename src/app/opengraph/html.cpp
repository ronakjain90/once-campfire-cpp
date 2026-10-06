// libxml2's HTML parser, as far as `<meta>` goes. Rust: crates/campfire/src/integrations/opengraph/html.rs.
#include "app/opengraph/html.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace campfire::app::opengraph {

namespace {

struct EntityEntry {
  std::string_view name;
  std::uint32_t value;
};
constexpr std::array kEntities = std::to_array<EntityEntry>({
#include "app/opengraph/entities.inc"
});

// How many attributes of one tag are kept; the rest are parsed and dropped.
constexpr std::size_t kMaxAttributes = 256;

bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

void append_utf8(std::string& out, std::uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// The length of the valid UTF-8 sequence at the start of `s` (0 if the first byte starts none).
std::size_t utf8_sequence(std::string_view s) {
  const auto b = [&](std::size_t i) { return static_cast<unsigned char>(s[i]); };
  const auto cont = [&](std::size_t i) { return i < s.size() && (b(i) & 0xC0) == 0x80; };
  const unsigned char c = b(0);
  if (c < 0x80) return 1;
  if (c >= 0xC2 && c <= 0xDF) return cont(1) ? 2 : 0;
  if (c >= 0xE0 && c <= 0xEF) {
    if (!cont(1) || !cont(2)) return 0;
    if (c == 0xE0 && b(1) < 0xA0) return 0;
    if (c == 0xED && b(1) >= 0xA0) return 0;
    return 3;
  }
  if (c >= 0xF0 && c <= 0xF4) {
    if (!cont(1) || !cont(2) || !cont(3)) return 0;
    if (c == 0xF0 && b(1) < 0x90) return 0;
    if (c == 0xF4 && b(1) >= 0x90) return 0;
    return 4;
  }
  return 0;
}

class Scanner {
 public:
  explicit Scanner(std::string_view bytes) : bytes_(bytes) {}

  std::vector<Element> run() {
    std::vector<Element> metas;
    while (const auto c = peek(0)) {
      if (*c != '<') {
        ++pos_;
        continue;
      }
      const auto next = peek(1);
      if (next == '/') {
        end_tag();
      } else if (next == '!') {
        markup_declaration();
      } else if (next == '?') {
        skip_past('>');
      } else if (next && is_alpha(*next)) {
        auto [name, element, self_closing] = start_tag();
        if (name == "meta") {
          metas.push_back(std::move(element));
        } else if ((name == "script" || name == "style") && !self_closing) {
          raw_text(name);
        }
      } else {
        ++pos_;
      }
    }
    return metas;
  }

 private:
  [[nodiscard]] std::optional<char> peek(std::size_t ahead) const {
    if (pos_ + ahead >= bytes_.size()) return std::nullopt;
    return bytes_[pos_ + ahead];
  }

  [[nodiscard]] bool starts_with_ignore_case(std::string_view text) const {
    if (pos_ + text.size() > bytes_.size()) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (lower(bytes_[pos_ + i]) != lower(text[i])) return false;
    }
    return true;
  }

  void skip_blanks() {
    while (peek(0) && is_blank(*peek(0))) ++pos_;
  }

  // Moves past the next `c` (or to the end).
  void skip_past(char c) {
    while (const auto next = peek(0)) {
      ++pos_;
      if (*next == c) break;
    }
  }

  // `htmlParseHTMLName`: `[A-Za-z_:.][A-Za-z0-9:_.-]*`, lowercased.
  std::optional<std::string> html_name() {
    const auto first = peek(0);
    if (!first || !(is_alpha(*first) || *first == '_' || *first == ':' || *first == '.')) return std::nullopt;
    std::string name;
    while (const auto c = peek(0)) {
      if (!(is_alnum(*c) || *c == ':' || *c == '-' || *c == '_' || *c == '.')) break;
      name += lower(*c);
      ++pos_;
    }
    return name;
  }

  void end_tag() {
    pos_ += 2;
    if (html_name()) skip_past('>');
  }

  // `<!--...-->` (with `<!-->` and `<!--->` closing at once, and `--!>` accepted), `<!DOCTYPE...>`, and any other
  // `<!...>` skipped as a bogus comment.
  void markup_declaration() {
    if (peek(2) == '-' && peek(3) == '-') {
      pos_ += 4;
      if (peek(0) == '>') {
        ++pos_;
        return;
      }
      if (peek(0) == '-' && peek(1) == '>') {
        pos_ += 2;
        return;
      }
      while (pos_ < bytes_.size()) {
        if (starts_with_ignore_case("-->")) {
          pos_ += 3;
          return;
        }
        if (starts_with_ignore_case("--!>")) {
          pos_ += 4;
          return;
        }
        ++pos_;
      }
    } else {
      skip_past('>');
    }
  }

  struct Tag {
    std::string name;
    Element element;
    bool self_closing;
  };

  // `htmlParseStartTag`
  Tag start_tag() {
    ++pos_;
    std::string name = html_name().value_or("");
    Element element;
    skip_blanks();
    for (;;) {
      const auto c = peek(0);
      if (!c || *c == '>' || (*c == '/' && peek(1) == '>')) break;
      if (auto attribute = html_name()) {
        skip_blanks();
        std::string value;
        if (peek(0) == '=') {
          ++pos_;
          skip_blanks();
          value = attribute_value();
        }
        const bool seen = std::any_of(element.attributes.begin(), element.attributes.end(),
                                      [&](const auto& entry) { return entry.first == *attribute; });
        if (element.attributes.size() < kMaxAttributes && !seen) {
          element.attributes.emplace_back(std::move(*attribute), std::move(value));
        }
      } else {
        // Dump the bogus attribute string up to the next blank or the end of the tag.
        while (const auto d = peek(0)) {
          if (is_blank(*d) || *d == '>' || (*d == '/' && peek(1) == '>')) break;
          ++pos_;
        }
      }
      skip_blanks();
    }
    const bool self_closing = peek(0) == '/';
    if (self_closing) {
      pos_ += 2;
    } else if (peek(0) == '>') {
      ++pos_;
    }
    return {std::move(name), std::move(element), self_closing};
  }

  // `htmlParseAttValue`
  std::string attribute_value() {
    const auto c = peek(0);
    if (c == '"' || c == '\'') {
      const char quote = *c;
      ++pos_;
      std::string value = attribute_text(quote);
      if (peek(0) == quote) ++pos_;
      return value;
    }
    return attribute_text(std::nullopt);
  }

  // `htmlParseHTMLAttribute`: up to the quote, or (unquoted) a blank or `>`.
  std::string attribute_text(std::optional<char> stop) {
    const auto ends_text = [&](char c) {
      return c == '&' || stop == c || (!stop && (c == '>' || is_blank(c)));
    };
    std::string out;
    bool truncated = false;
    for (;;) {
      const std::size_t start = pos_;
      while (peek(0) && !ends_text(*peek(0))) ++pos_;
      if (!truncated) out.append(bytes_.substr(start, pos_ - start));
      if (peek(0) != '&') break;
      std::optional<std::string> decoded;
      if (peek(1) == '#') {
        if (const auto cp = char_ref()) {
          std::string text;
          append_utf8(text, *cp);
          decoded = std::move(text);
        }
      } else {
        decoded = entity_ref();
      }
      if (!decoded) {
        truncated = true;
      } else if (!truncated) {
        out += *decoded;
      }
    }
    return out;
  }

  // `htmlParseCharRef`: nothing for a value that is not a valid XML character.
  std::optional<std::uint32_t> char_ref() {
    const bool hex = peek(2) == 'x' || peek(2) == 'X';
    pos_ += hex ? 3 : 2;
    const std::uint32_t radix = hex ? 16 : 10;
    std::uint32_t value = 0;
    while (const auto c = peek(0)) {
      if (*c == ';') {
        ++pos_;
        break;
      }
      std::uint32_t digit;
      if (is_digit(*c)) {
        digit = static_cast<std::uint32_t>(*c - '0');
      } else if (hex && *c >= 'a' && *c <= 'f') {
        digit = static_cast<std::uint32_t>(*c - 'a' + 10);
      } else if (hex && *c >= 'A' && *c <= 'F') {
        digit = static_cast<std::uint32_t>(*c - 'A' + 10);
      } else {
        break;
      }
      if (value < 0x110000) value = value * radix + digit;
      ++pos_;
    }
    const bool is_char = value == 0x9 || value == 0xA || value == 0xD || (value >= 0x20 && value <= 0xD7FF) ||
                         (value >= 0xE000 && value <= 0xFFFD) || (value >= 0x10000 && value <= 0x10FFFF);
    if (!is_char) return std::nullopt;
    return value;
  }

  // `htmlParseEntityRef`: a known name followed by `;` decodes. Anything else stays as written.
  std::string entity_ref() {
    ++pos_;
    const std::size_t start = pos_;
    if (peek(0) && (is_alpha(*peek(0)) || *peek(0) == '_' || *peek(0) == ':')) {
      while (peek(0) && (is_alnum(*peek(0)) || *peek(0) == '_' || *peek(0) == ':' || *peek(0) == '.' || *peek(0) == '-')) {
        ++pos_;
      }
    }
    const std::string_view name = bytes_.substr(start, pos_ - start);
    if (!name.empty() && peek(0) == ';') {
      const auto it = std::lower_bound(kEntities.begin(), kEntities.end(), name,
                                       [](const EntityEntry& entry, std::string_view n) { return entry.name < n; });
      if (it != kEntities.end() && it->name == name) {
        ++pos_;
        std::string text;
        append_utf8(text, it->value);
        return text;
      }
    }
    return "&" + std::string(name);
  }

  // `htmlParseScript`: everything up to `</name` (any case) is text.
  void raw_text(const std::string& name) {
    const std::string end = "</" + name;
    while (pos_ < bytes_.size() && !starts_with_ignore_case(end)) ++pos_;
  }

  std::string_view bytes_;
  std::size_t pos_ = 0;
};

// `content[/charset\s*=\s*([\w-]+)/i, 1]`
std::optional<std::string> charset_in(std::string_view content) {
  const auto space = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
  for (std::size_t i = 0; i + 7 <= content.size(); ++i) {
    std::string word;
    for (std::size_t k = 0; k < 7; ++k) word += lower(content[i + k]);
    if (word != "charset") continue;
    std::size_t j = i + 7;
    while (j < content.size() && space(content[j])) ++j;
    if (j >= content.size() || content[j] != '=') continue;
    ++j;
    while (j < content.size() && space(content[j])) ++j;
    std::size_t k = j;
    while (k < content.size() && (is_alnum(content[k]) || content[k] == '_' || content[k] == '-')) ++k;
    if (k > j) return std::string(content.substr(j, k - j));
  }
  return std::nullopt;
}

}  // namespace

const std::string* Element::attr(std::string_view name) const {
  for (const auto& [key, value] : attributes) {
    if (key == name) return &value;
  }
  return nullptr;
}

std::string decode(std::string_view bytes) {
  std::string out;
  out.reserve(bytes.size());
  while (!bytes.empty()) {
    const std::size_t length = utf8_sequence(bytes);
    if (length > 0) {
      out.append(bytes.substr(0, length));
      bytes.remove_prefix(length);
    } else {
      append_utf8(out, static_cast<unsigned char>(bytes.front()));
      bytes.remove_prefix(1);
    }
  }
  return out;
}

std::vector<Element> meta_elements(std::string_view html) {
  // A NUL ends libxml2's input.
  html = html.substr(0, html.find('\0'));
  return Scanner(html).run();
}

std::optional<std::string> meta_encoding(const std::vector<Element>& metas) {
  for (const Element& meta : metas) {
    if (const auto* charset = meta.attr("charset")) return *charset;
  }
  for (const Element& meta : metas) {
    const auto* content = meta.attr("content");
    const auto* equiv = meta.attr("http-equiv");
    if (content == nullptr || equiv == nullptr) continue;
    std::string lowered;
    for (const char c : *equiv) lowered += lower(c);
    if (lowered == "content-type") return charset_in(*content);
  }
  return std::nullopt;
}

}  // namespace campfire::app::opengraph
