// The JSON Rails writes: the json gem's JSON.generate and ActiveSupport::JSON.encode
// (Rust: crates/rails_compat/src/json.rs). Objects keep insertion order, as Ruby hashes do.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace campfire::compat::json {

class Value {
 public:
  using Array = std::vector<Value>;
  using Member = std::pair<std::string, Value>;
  using Object = std::vector<Member>;

  Value() : data_(nullptr) {}
  Value(std::nullptr_t) : data_(nullptr) {}
  Value(bool b) : data_(b) {}
  Value(int n) : data_(int64_t(n)) {}
  Value(int64_t n) : data_(n) {}
  // Only a number above INT64_MAX uses the unsigned alternative.
  Value(uint64_t n) { *this = from_unsigned(n); }
  Value(double d) : data_(d) {}
  Value(std::string s) : data_(std::move(s)) {}
  Value(std::string_view s) : data_(std::string(s)) {}
  Value(const char* s) : data_(std::string(s)) {}
  Value(Array a) : data_(std::move(a)) {}
  Value(Object o) : data_(std::move(o)) {}

  static Value from_unsigned(uint64_t n) {
    Value v;
    if (n <= uint64_t(INT64_MAX)) v.data_ = int64_t(n);
    else v.data_ = n;
    return v;
  }

  bool is_null() const { return std::holds_alternative<std::nullptr_t>(data_); }
  bool is_bool() const { return std::holds_alternative<bool>(data_); }
  bool is_int() const { return std::holds_alternative<int64_t>(data_); }
  bool is_uint() const { return std::holds_alternative<uint64_t>(data_); }
  bool is_double() const { return std::holds_alternative<double>(data_); }
  bool is_number() const { return is_int() || is_uint() || is_double(); }
  bool is_string() const { return std::holds_alternative<std::string>(data_); }
  bool is_array() const { return std::holds_alternative<Array>(data_); }
  bool is_object() const { return std::holds_alternative<Object>(data_); }

  bool as_bool() const { return std::get<bool>(data_); }
  const std::string& as_string() const { return std::get<std::string>(data_); }
  const Array& as_array() const { return std::get<Array>(data_); }
  Array& as_array() { return std::get<Array>(data_); }
  const Object& as_object() const { return std::get<Object>(data_); }
  Object& as_object() { return std::get<Object>(data_); }
  double as_double() const { return std::get<double>(data_); }

  // The value as an int64 when it is an integer that fits. A double never converts.
  std::optional<int64_t> to_int64() const {
    if (auto* p = std::get_if<int64_t>(&data_)) return *p;
    return std::nullopt;
  }
  const std::string* get_string() const { return std::get_if<std::string>(&data_); }
  const uint64_t* get_uint() const { return std::get_if<uint64_t>(&data_); }

  // Object member by key, or nullptr (also for a value that is not an object).
  const Value* find(std::string_view key) const;
  // Hash#[]=: replaces a member in place or appends it. The value must be an object.
  void set(std::string key, Value value);

  friend bool operator==(const Value& a, const Value& b) { return a.data_ == b.data_; }

 private:
  std::variant<std::nullptr_t, bool, int64_t, uint64_t, double, std::string, Array, Object> data_;
};

struct ParseOptions {
  // Ruby's JSON.parse also skips /* */ and // comments. serde_json, which Rails verifiers match, does not.
  bool allow_comments = false;
};

// Parses one JSON document. Returns nullopt for invalid JSON, invalid UTF-8, lone surrogates,
// trailing data or nesting deeper than 128. A repeated key keeps the last value at the
// position of the first one.
std::optional<Value> parse(std::string_view text, ParseOptions options = {});

// ::JSON.generate / JSON.dump: compact, non-ASCII left as UTF-8. A non-finite float is "null"
// (what ActiveSupport does for Float#as_json). Invalid UTF-8 in a string becomes U+FFFD.
std::string generate(const Value& value);

// ActiveSupport::JSON.encode with escape_html_entities_in_json: like generate, plus
// "<", ">" and "&" written as <, > and &. U+2028 and U+2029 stay raw.
std::string encode(const Value& value);

// Re-escapes JSON text that is already encoded.
std::string escape_html_entities(std::string_view json);

// A float as the json gem writes it (not Float#to_s): 1e15 is "1e+15" and 1e-5 is "0.00001".
std::string float_to_json(double value);

}  // namespace campfire::compat::json
