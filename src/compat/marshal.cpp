// Marshal subset (see marshal.hpp).
#include "compat/marshal.hpp"

#include <algorithm>
#include <cstdlib>

namespace campfire::compat::marshal {
namespace {

// Reads a Marshal "long" (r_long in marshal.c). Returns the value and advances `rest`.
std::optional<int64_t> read_fixnum(std::string_view& rest) {
  if (rest.empty()) return std::nullopt;
  int8_t first = int8_t(rest[0]);
  rest.remove_prefix(1);
  if (first == 0) return 0;
  if (first >= 5 && first <= 127) return int64_t(first) - 5;
  if (first <= -5) return int64_t(first) + 5;
  size_t n = size_t(first > 0 ? first : -first);
  if (rest.size() < n) return std::nullopt;
  int64_t value;
  if (first > 0) {
    value = 0;
    for (size_t i = n; i-- > 0;) value = (value << 8) | uint8_t(rest[i]);
  } else {
    value = -1;
    for (size_t i = 0; i < n; ++i) {
      value &= ~(int64_t(0xff) << (8 * i));
      value |= int64_t(uint8_t(rest[i])) << (8 * i);
    }
  }
  rest.remove_prefix(n);
  return value;
}

class Writer {
 public:
  std::string out{"\x04\x08", 2};

  void value(const Value& v) {
    std::visit(
        [&](const auto& x) {
          using T = std::decay_t<decltype(x)>;
          if constexpr (std::is_same_v<T, Value::Nil>) {
            out += '0';
          } else if constexpr (std::is_same_v<T, bool>) {
            out += x ? 'T' : 'F';
          } else if constexpr (std::is_same_v<T, int64_t>) {
            integer(x);
          } else if constexpr (std::is_same_v<T, Value::Symbol>) {
            symbol(x.name);
          } else if constexpr (std::is_same_v<T, Value::Str>) {
            // A string with an encoding is wrapped in an ivar list holding E: true (UTF-8).
            out += 'I';
            out += '"';
            bytes(x.text);
            long_(1);
            symbol("E");
            out += 'T';
          } else if constexpr (std::is_same_v<T, Value::Array>) {
            out += '[';
            long_(int64_t(x.size()));
            for (const auto& item : x) value(item);
          } else {
            out += '{';
            long_(int64_t(x.size()));
            for (const auto& [key, item] : x) {
              symbol(key);
              value(item);
            }
          }
        },
        v.variant());
  }

 private:
  std::vector<std::string> symbols_;

  // Integers whose tagged VALUE fits in 31 bits are written inline (i), larger ones as bignums (l).
  void integer(int64_t n) {
    if (n >= -(int64_t(1) << 30) && n < (int64_t(1) << 30)) {
      out += 'i';
      long_(n);
      return;
    }
    out += 'l';
    out += n < 0 ? '-' : '+';
    uint64_t magnitude = n < 0 ? uint64_t(0) - uint64_t(n) : uint64_t(n);
    std::string digits;
    while (magnitude > 0) {
      digits += char(magnitude & 0xff);
      magnitude >>= 8;
    }
    if (digits.size() % 2 == 1) digits += '\0';
    long_(int64_t(digits.size() / 2));
    out += digits;
  }

  void symbol(const std::string& name) {
    auto it = std::find(symbols_.begin(), symbols_.end(), name);
    if (it != symbols_.end()) {
      out += ';';
      long_(it - symbols_.begin());
    } else {
      symbols_.push_back(name);
      out += ':';
      bytes(name);
    }
  }

  void bytes(std::string_view b) {
    long_(int64_t(b.size()));
    out.append(b);
  }

  // w_long in marshal.c.
  void long_(int64_t n) {
    if (n == 0) {
      out += '\0';
    } else if (0 < n && n < 123) {
      out += char(n + 5);
    } else if (-124 < n && n < 0) {
      out += char((n - 5) & 0xff);
    } else {
      char buf[9];
      int64_t x = n;
      for (int i = 1; i <= 8; ++i) {
        buf[i] = char(x & 0xff);
        x >>= 8;
        if (x == 0) {
          buf[0] = char(i);
          out.append(buf, size_t(i) + 1);
          return;
        }
        if (x == -1) {
          buf[0] = char(-i);
          out.append(buf, size_t(i) + 1);
          return;
        }
      }
    }
  }
};

}  // namespace

std::optional<std::string> load_string(std::string_view dumped) {
  if (!dumped.starts_with(kSignature)) return std::nullopt;
  std::string_view rest = dumped.substr(kSignature.size());
  if (rest.starts_with('I')) rest.remove_prefix(1);
  if (!rest.starts_with('"')) return std::nullopt;
  rest.remove_prefix(1);
  auto len = read_fixnum(rest);
  if (!len || *len < 0 || size_t(*len) > rest.size()) return std::nullopt;
  return std::string(rest.substr(0, size_t(*len)));
}

std::string dump(const Value& value) {
  Writer w;
  w.value(value);
  return std::move(w.out);
}

}  // namespace campfire::compat::marshal
