// The subset of Ruby's Marshal format (4.8) Campfire needs.
// Load: a marshaled String, the payload of Rails 7 signed messages (Rust: rails_compat/src/marshal.rs).
// Dump: transformation hashes, for ActiveStorage::Variation#digest (Rust: storage/src/marshal.rs).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace campfire::compat::marshal {

inline constexpr std::string_view kSignature = "\x04\x08";

// Loads `"\x04\x08" ["I"] '"' <len> <bytes> [<ivars>]`. Anything else gives nullopt.
std::optional<std::string> load_string(std::string_view dumped);

// A Ruby value as it appears in a transformations hash. Symbols and strings are distinct:
// `format: :webp` and `format: "webp"` digest differently.
class Value {
 public:
  struct Nil {};
  struct Symbol {
    std::string name;
  };
  struct Str {
    std::string text;  // UTF-8
  };
  using Array = std::vector<Value>;
  using Hash = std::vector<std::pair<std::string, Value>>;  // symbol keys, insertion order

  Value() : data_(Nil{}) {}
  static Value nil() { return Value(); }
  static Value boolean(bool b) { return Value(b); }
  static Value integer(int64_t n) { return Value(n); }
  static Value symbol(std::string name) { return Value(Symbol{std::move(name)}); }
  static Value string(std::string text) { return Value(Str{std::move(text)}); }
  static Value array(Array items) { return Value(std::move(items)); }
  static Value hash(Hash entries) { return Value(std::move(entries)); }

  const auto& variant() const { return data_; }
  friend bool operator==(const Value& a, const Value& b) { return a.data_ == b.data_; }
  friend bool operator==(const Symbol& a, const Symbol& b) { return a.name == b.name; }
  friend bool operator==(const Str& a, const Str& b) { return a.text == b.text; }
  friend bool operator==(const Nil&, const Nil&) { return true; }

 private:
  template <typename T>
  explicit Value(T v) : data_(std::move(v)) {}
  std::variant<Nil, bool, int64_t, Symbol, Str, Array, Hash> data_;
};

// Marshal.dump(value).
std::string dump(const Value& value);

}  // namespace campfire::compat::marshal
