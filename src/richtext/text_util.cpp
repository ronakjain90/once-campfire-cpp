// Rails: active_support/core_ext/object/blank.rb, Ruby String and Object#inspect. Rust: crates/richtext/src/ruby.rs
#include "richtext/text_util.hpp"

#include "compat/ruby.hpp"
#include "richtext/sanitizer.hpp"

namespace campfire::richtext {

char32_t next_code_point(std::string_view text, std::size_t& pos) noexcept {
  const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
  const unsigned char b0 = byte(pos);
  std::size_t length = 1;
  char32_t cp = b0;
  if (b0 >= 0xC2 && b0 <= 0xDF) {
    length = 2;
    cp = b0 & 0x1Fu;
  } else if (b0 >= 0xE0 && b0 <= 0xEF) {
    length = 3;
    cp = b0 & 0x0Fu;
  } else if (b0 >= 0xF0 && b0 <= 0xF4) {
    length = 4;
    cp = b0 & 0x07u;
  } else if (b0 >= 0x80) {
    ++pos;
    return 0xFFFD;
  }
  if (length > 1) {
    if (pos + length > text.size()) {
      ++pos;
      return 0xFFFD;
    }
    for (std::size_t i = 1; i < length; ++i) {
      if ((byte(pos + i) & 0xC0u) != 0x80u) {
        ++pos;
        return 0xFFFD;
      }
      cp = (cp << 6) | (byte(pos + i) & 0x3Fu);
    }
  }
  pos += length;
  return cp;
}

void append_utf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

bool is_blank(std::string_view text) noexcept {
  for (std::size_t pos = 0; pos < text.size();) {
    if (!is_unicode_space(next_code_point(text, pos))) {
      return false;
    }
  }
  return true;
}

std::string_view chomp_newlines(std::string_view text) noexcept {
  while (true) {
    if (text.ends_with("\r\n")) {
      text.remove_suffix(2);
    } else if (text.ends_with('\n')) {
      text.remove_suffix(1);
    } else {
      return text;
    }
  }
}

std::string_view chomp(std::string_view text) noexcept {
  if (text.ends_with("\r\n")) {
    text.remove_suffix(2);
  } else if (text.ends_with('\n') || text.ends_with('\r')) {
    text.remove_suffix(1);
  }
  return text;
}

namespace {

std::size_t code_point_count(std::string_view text) noexcept {
  std::size_t count = 0;
  for (std::size_t pos = 0; pos < text.size(); ++count) {
    (void)next_code_point(text, pos);
  }
  return count;
}

}  // namespace

std::string truncate(std::string_view text, std::size_t length, std::string_view omission) {
  if (code_point_count(text) <= length) {
    return std::string(text);
  }
  const std::size_t omission_length = code_point_count(omission);
  const std::size_t keep = length > omission_length ? length - omission_length : 0;
  std::size_t pos = 0;
  for (std::size_t i = 0; i < keep && pos < text.size(); ++i) {
    (void)next_code_point(text, pos);
  }
  std::string out(text.substr(0, pos));
  out.append(omission);
  return out;
}

namespace {

std::string string_inspect(std::string_view text) {
  std::string out = "\"";
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    const char next = i + 1 < text.size() ? text[i + 1] : '\0';
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      case '\f': out += "\\f"; break;
      case '\v': out += "\\v"; break;
      case '\b': out += "\\b"; break;
      case '\a': out += "\\a"; break;
      case '\x1b': out += "\\e"; break;
      case '#':
        // What would start an interpolation in a double-quoted literal: `#{`, `#$`, `#@`.
        out += (next == '{' || next == '$' || next == '@') ? "\\#" : "#";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
          static constexpr char kHex[] = "0123456789ABCDEF";
          out += "\\x";
          out.push_back(kHex[(static_cast<unsigned char>(c) >> 4) & 0xF]);
          out.push_back(kHex[static_cast<unsigned char>(c) & 0xF]);
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
  return out;
}

}  // namespace

std::string json_value_inspect(const compat::json::Value& value) {
  if (value.is_null()) {
    return "nil";
  }
  if (value.is_bool()) {
    return value.as_bool() ? "true" : "false";
  }
  if (auto n = value.to_int64()) {
    return std::to_string(*n);
  }
  if (const auto* u = value.get_uint()) {
    return std::to_string(*u);
  }
  if (value.is_double()) {
    return compat::float_to_s(value.as_double());
  }
  if (const auto* s = value.get_string()) {
    return string_inspect(*s);
  }
  if (value.is_array()) {
    std::string out = "[";
    bool first = true;
    for (const auto& item : value.as_array()) {
      out += first ? "" : ", ";
      first = false;
      out += json_value_inspect(item);
    }
    return out + "]";
  }
  const auto& members = value.as_object();
  if (members.empty()) {
    return "{}";
  }
  std::string out = "{";
  bool first = true;
  for (const auto& [key, member] : members) {
    out += first ? "" : ", ";
    first = false;
    out += string_inspect(key) + " => " + json_value_inspect(member);
  }
  return out + "}";
}

std::string json_value_to_s(const compat::json::Value& value) {
  if (const auto* s = value.get_string()) {
    return *s;
  }
  if (value.is_null()) {
    return "";
  }
  return json_value_inspect(value);
}

}  // namespace campfire::richtext
