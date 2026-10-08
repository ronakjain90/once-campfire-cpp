// The JSON Rails writes (see json.hpp). Rust: crates/rails_compat/src/json.rs.
#include "compat/json.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "compat/float_digits.hpp"

namespace campfire::compat::json {

const Value* Value::find(std::string_view key) const {
  const auto* object = std::get_if<Object>(&data_);
  if (object == nullptr) return nullptr;
  for (const auto& [k, v] : *object) {
    if (k == key) return &v;
  }
  return nullptr;
}

void Value::set(std::string key, Value value) {
  auto& object = std::get<Object>(data_);
  for (auto& member : object) {
    if (member.first == key) {
      member.second = std::move(value);
      return;
    }
  }
  object.emplace_back(std::move(key), std::move(value));
}

// ---- generation -----------------------------------------------------------------------------

namespace {

// Length of the valid UTF-8 sequence at s[i], or 0.
size_t utf8_length(std::string_view s, size_t i) {
  auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
  unsigned char c = b(i);
  if (c < 0x80) return 1;
  auto cont = [&](size_t k) { return k < s.size() && (b(k) & 0xC0) == 0x80; };
  if (c >= 0xC2 && c <= 0xDF) return cont(i + 1) ? 2 : 0;
  if (c >= 0xE0 && c <= 0xEF) {
    if (!cont(i + 1) || !cont(i + 2)) return 0;
    if (c == 0xE0 && b(i + 1) < 0xA0) return 0;
    if (c == 0xED && b(i + 1) >= 0xA0) return 0;  // surrogates
    return 3;
  }
  if (c >= 0xF0 && c <= 0xF4) {
    if (!cont(i + 1) || !cont(i + 2) || !cont(i + 3)) return 0;
    if (c == 0xF0 && b(i + 1) < 0x90) return 0;
    if (c == 0xF4 && b(i + 1) >= 0x90) return 0;
    return 4;
  }
  return 0;
}

// The bytes that `write_string` must look at: control characters, `"`, `\\`, the start of UTF-8 sequences, and (for
// `Html`) `<`, `>` and `&`. The others are copied as they are.
template <bool Html>
constexpr std::array<bool, 256> make_special() {
  std::array<bool, 256> table{};
  for (int c = 0; c < 256; ++c) table[static_cast<size_t>(c)] = c < 0x20 || c >= 0x80 || c == '"' || c == '\\';
  if (Html) table['<'] = table['>'] = table['&'] = true;
  return table;
}

// `Html`: also escape `<`, `>` and `&` as ActiveSupport::JSON does (`escape_html_entities` in one pass).
template <bool Html>
void write_string(std::string& out, std::string_view s) {
  static constexpr char kHex[] = "0123456789abcdef";
  static constexpr std::array<bool, 256> kSpecial = make_special<Html>();
  out.reserve(out.size() + s.size() + 2);
  out += '"';
  size_t run = 0;  // start of the bytes not yet copied
  size_t i = 0;
  auto flush = [&](size_t end) { out.append(s.data() + run, end - run); };
  while (i < s.size()) {
    // Skip the plain bytes first: most of a message's HTML.
    while (i < s.size() && !kSpecial[static_cast<unsigned char>(s[i])]) ++i;
    if (i == s.size()) break;
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0x80) {
      size_t n = utf8_length(s, i);
      if (n == 0) {
        flush(i);
        out += "\xEF\xBF\xBD";
        ++i;
        run = i;
      } else {
        i += n;
      }
      continue;
    }
    const char* replacement = nullptr;
    char unicode[7];
    switch (c) {
      case '"': replacement = "\\\""; break;
      case '\\': replacement = "\\\\"; break;
      case '\b': replacement = "\\b"; break;
      case '\f': replacement = "\\f"; break;
      case '\n': replacement = "\\n"; break;
      case '\r': replacement = "\\r"; break;
      case '\t': replacement = "\\t"; break;
      case '<':
        if constexpr (Html) replacement = "\\u003c";
        break;
      case '>':
        if constexpr (Html) replacement = "\\u003e";
        break;
      case '&':
        if constexpr (Html) replacement = "\\u0026";
        break;
      default:
        if (c < 0x20) {
          unicode[0] = '\\';
          unicode[1] = 'u';
          unicode[2] = '0';
          unicode[3] = '0';
          unicode[4] = kHex[c >> 4];
          unicode[5] = kHex[c & 15];
          unicode[6] = '\0';
          replacement = unicode;
        }
    }
    if (replacement != nullptr) {
      flush(i);
      out += replacement;
      run = i + 1;
    }
    ++i;
  }
  flush(s.size());
  out += '"';
}

template <bool Html>
void write_value(std::string& out, const Value& v);

}  // namespace

std::string float_to_json(double f) {
  std::string sign = std::signbit(f) ? "-" : "";
  if (f == 0.0) return sign + "0.0";
  detail::Digits d = detail::shortest_digits(std::fabs(f));
  const std::string& digits = d.digits;
  int exponent = d.decpt - 1;
  int k = exponent + 1 - int(digits.size());  // power of ten of the last digit (fpconv's K)
  std::string body;
  if (k >= 0 && exponent < 15) {
    body = digits + std::string(size_t(k), '0') + ".0";
  } else if (k < 0 && (k > -7 || std::abs(exponent) < 10)) {
    int point = exponent + 1;
    if (point <= 0) {
      body = "0." + std::string(size_t(-point), '0') + digits;
    } else {
      body = digits.substr(0, size_t(point)) + "." + digits.substr(size_t(point));
    }
  } else {
    body = digits.substr(0, 1);
    if (digits.size() > 1) body += "." + digits.substr(1);
    body += 'e';
    body += exponent < 0 ? '-' : '+';
    body += std::to_string(std::abs(exponent));
  }
  return sign + body;
}

namespace {

template <bool Html>
void write_value(std::string& out, const Value& v) {
  if (v.is_null()) {
    out += "null";
  } else if (v.is_bool()) {
    out += v.as_bool() ? "true" : "false";
  } else if (auto i = v.to_int64()) {
    out += std::to_string(*i);
  } else if (const uint64_t* u = v.get_uint()) {
    out += std::to_string(*u);
  } else if (v.is_double()) {
    double d = v.as_double();
    out += std::isfinite(d) ? float_to_json(d) : "null";
  } else if (const std::string* s = v.get_string()) {
    write_string<Html>(out, *s);
  } else if (v.is_array()) {
    out += '[';
    bool first = true;
    for (const auto& item : v.as_array()) {
      if (!first) out += ',';
      first = false;
      write_value<Html>(out, item);
    }
    out += ']';
  } else {
    out += '{';
    bool first = true;
    for (const auto& [key, item] : v.as_object()) {
      if (!first) out += ',';
      first = false;
      write_string<Html>(out, key);
      out += ':';
      write_value<Html>(out, item);
    }
    out += '}';
  }
}

}  // namespace

std::string generate(const Value& value) {
  std::string out;
  out.reserve(128);
  write_value<false>(out, value);
  return out;
}

bool valid_utf8(std::string_view text) {
  for (size_t i = 0; i < text.size();) {
    size_t n = utf8_length(text, i);
    if (n == 0) return false;
    i += n;
  }
  return true;
}

std::string escape_html_entities(std::string_view json) {
  std::string out;
  out.reserve(json.size() + 16);
  for (char c : json) {
    switch (c) {
      case '<': out += "\\u003c"; break;
      case '>': out += "\\u003e"; break;
      case '&': out += "\\u0026"; break;
      default: out += c;
    }
  }
  return out;
}

std::string encode(const Value& value) {
  // The same bytes as `escape_html_entities(generate(value))`: `<`, `>` and `&` only occur inside strings.
  std::string out;
  out.reserve(128);
  write_value<true>(out, value);
  return out;
}

// ---- parsing --------------------------------------------------------------------------------

namespace {

constexpr int kMaxDepth = 128;

class Parser {
 public:
  Parser(std::string_view text, ParseOptions options) : s_(text), options_(options) {}

  std::optional<Value> run() {
    if (!skip_space()) return std::nullopt;
    auto value = parse_value(0);
    if (!value) return std::nullopt;
    if (!skip_space()) return std::nullopt;
    if (pos_ != s_.size()) return std::nullopt;
    return value;
  }

 private:
  std::string_view s_;
  ParseOptions options_;
  size_t pos_ = 0;

  bool at_end() const { return pos_ >= s_.size(); }
  char peek() const { return s_[pos_]; }

  // Skips whitespace, and comments when allowed. False means an unterminated comment.
  bool skip_space() {
    while (!at_end()) {
      char c = peek();
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else if (options_.allow_comments && c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '*') {
        size_t end = s_.find("*/", pos_ + 2);
        if (end == std::string_view::npos) return false;
        pos_ = end + 2;
      } else if (options_.allow_comments && c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '/') {
        size_t end = s_.find('\n', pos_ + 2);
        pos_ = end == std::string_view::npos ? s_.size() : end + 1;
      } else {
        break;
      }
    }
    return true;
  }

  bool consume(std::string_view word) {
    if (s_.substr(pos_, word.size()) != word) return false;
    pos_ += word.size();
    return true;
  }

  std::optional<Value> parse_value(int depth) {
    if (at_end()) return std::nullopt;
    switch (peek()) {
      case 'n': return consume("null") ? std::optional<Value>(Value(nullptr)) : std::nullopt;
      case 't': return consume("true") ? std::optional<Value>(Value(true)) : std::nullopt;
      case 'f': return consume("false") ? std::optional<Value>(Value(false)) : std::nullopt;
      case '"': {
        auto s = parse_string();
        if (!s) return std::nullopt;
        return Value(std::move(*s));
      }
      case '[': return parse_array(depth);
      case '{': return parse_object(depth);
      default: return parse_number();
    }
  }

  std::optional<Value> parse_array(int depth) {
    if (depth + 1 > kMaxDepth) return std::nullopt;
    ++pos_;
    Value::Array items;
    if (!skip_space()) return std::nullopt;
    if (!at_end() && peek() == ']') {
      ++pos_;
      return Value(std::move(items));
    }
    while (true) {
      if (!skip_space()) return std::nullopt;
      auto item = parse_value(depth + 1);
      if (!item) return std::nullopt;
      items.push_back(std::move(*item));
      if (!skip_space() || at_end()) return std::nullopt;
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == ']') {
        ++pos_;
        return Value(std::move(items));
      }
      return std::nullopt;
    }
  }

  std::optional<Value> parse_object(int depth) {
    if (depth + 1 > kMaxDepth) return std::nullopt;
    ++pos_;
    Value::Object members;
    if (!skip_space()) return std::nullopt;
    if (!at_end() && peek() == '}') {
      ++pos_;
      return Value(std::move(members));
    }
    while (true) {
      if (!skip_space() || at_end() || peek() != '"') return std::nullopt;
      auto key = parse_string();
      if (!key) return std::nullopt;
      if (!skip_space() || at_end() || peek() != ':') return std::nullopt;
      ++pos_;
      if (!skip_space()) return std::nullopt;
      auto item = parse_value(depth + 1);
      if (!item) return std::nullopt;
      bool replaced = false;
      for (auto& member : members) {
        if (member.first == *key) {
          member.second = std::move(*item);
          replaced = true;
          break;
        }
      }
      if (!replaced) members.emplace_back(std::move(*key), std::move(*item));
      if (!skip_space() || at_end()) return std::nullopt;
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == '}') {
        ++pos_;
        return Value(std::move(members));
      }
      return std::nullopt;
    }
  }

  static void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out += char(cp);
    } else if (cp < 0x800) {
      out += char(0xC0 | (cp >> 6));
      out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += char(0xE0 | (cp >> 12));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    } else {
      out += char(0xF0 | (cp >> 18));
      out += char(0x80 | ((cp >> 12) & 0x3F));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    }
  }

  std::optional<uint32_t> hex4() {
    if (pos_ + 4 > s_.size()) return std::nullopt;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = s_[pos_ + size_t(i)];
      int d;
      if (c >= '0' && c <= '9')
        d = c - '0';
      else if (c >= 'a' && c <= 'f')
        d = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
        d = c - 'A' + 10;
      else
        return std::nullopt;
      v = v * 16 + uint32_t(d);
    }
    pos_ += 4;
    return v;
  }

  std::optional<std::string> parse_string() {
    ++pos_;  // opening quote
    std::string out;
    while (true) {
      if (at_end()) return std::nullopt;
      unsigned char c = static_cast<unsigned char>(peek());
      if (c == '"') {
        ++pos_;
        return out;
      }
      if (c < 0x20) return std::nullopt;
      if (c == '\\') {
        ++pos_;
        if (at_end()) return std::nullopt;
        char e = peek();
        ++pos_;
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            auto cp = hex4();
            if (!cp) return std::nullopt;
            if (*cp >= 0xD800 && *cp <= 0xDBFF) {
              if (!consume("\\u")) return std::nullopt;
              auto low = hex4();
              if (!low || *low < 0xDC00 || *low > 0xDFFF) return std::nullopt;
              append_utf8(out, 0x10000 + ((*cp - 0xD800) << 10) + (*low - 0xDC00));
            } else if (*cp >= 0xDC00 && *cp <= 0xDFFF) {
              return std::nullopt;
            } else {
              append_utf8(out, *cp);
            }
            break;
          }
          default: return std::nullopt;
        }
        continue;
      }
      if (c < 0x80) {
        out += char(c);
        ++pos_;
        continue;
      }
      size_t n = utf8_length(s_, pos_);
      if (n == 0) return std::nullopt;
      out.append(s_.data() + pos_, n);
      pos_ += n;
    }
  }

  std::optional<Value> parse_number() {
    size_t start = pos_;
    bool negative = false;
    if (!at_end() && peek() == '-') {
      negative = true;
      ++pos_;
    }
    if (at_end()) return std::nullopt;
    if (peek() == '0') {
      ++pos_;
    } else if (peek() >= '1' && peek() <= '9') {
      while (!at_end() && peek() >= '0' && peek() <= '9') ++pos_;
    } else {
      return std::nullopt;
    }
    bool is_float = false;
    if (!at_end() && peek() == '.') {
      ++pos_;
      size_t frac = pos_;
      while (!at_end() && peek() >= '0' && peek() <= '9') ++pos_;
      if (pos_ == frac) return std::nullopt;
      is_float = true;
    }
    if (!at_end() && (peek() == 'e' || peek() == 'E')) {
      ++pos_;
      if (!at_end() && (peek() == '+' || peek() == '-')) ++pos_;
      size_t exp = pos_;
      while (!at_end() && peek() >= '0' && peek() <= '9') ++pos_;
      if (pos_ == exp) return std::nullopt;
      is_float = true;
    }
    std::string_view text = s_.substr(start, pos_ - start);
    if (!is_float) {
      if (negative) {
        int64_t i;
        auto r = std::from_chars(text.data(), text.data() + text.size(), i);
        if (r.ec == std::errc() && r.ptr == text.data() + text.size()) return Value(i);
      } else {
        uint64_t u;
        auto r = std::from_chars(text.data(), text.data() + text.size(), u);
        if (r.ec == std::errc() && r.ptr == text.data() + text.size()) return Value::from_unsigned(u);
      }
      // An integer too large for 64 bits is read as a float.
    }
    double d;
    auto r = std::from_chars(text.data(), text.data() + text.size(), d);
    if (r.ec != std::errc() && r.ec != std::errc::result_out_of_range) return std::nullopt;
    if (r.ec == std::errc::result_out_of_range) {
      // Too large is an error (as in serde_json). Too small is zero.
      bool tiny = false;
      size_t e = text.find_first_of("eE");
      if (e != std::string_view::npos && text[e + 1] == '-') tiny = true;
      if (!tiny) return std::nullopt;
      d = negative ? -0.0 : 0.0;
    }
    if (!std::isfinite(d)) return std::nullopt;
    return Value(d);
  }
};

}  // namespace

std::optional<Value> parse(std::string_view text, ParseOptions options) {
  return Parser(text, options).run();
}

}  // namespace campfire::compat::json
