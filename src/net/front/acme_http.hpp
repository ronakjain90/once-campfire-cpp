// A small blocking HTTP/1.1 client with TLS, and a JSON reader, for the ACME client.
// Rust: crates/kit/src/front/acme.rs (instant_acme brings its own client).
#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "core/error.hpp"

namespace campfire::net::front {

struct HttpResult {
  int status = 0;
  std::vector<std::pair<std::string, std::string>> headers;  // names in lowercase
  std::string body;
  [[nodiscard]] std::string header(std::string_view lower_name) const;
};

// One request, one connection ("connection: close"). `ca_file` is a PEM file of roots to trust
// (empty: the system roots). The timeout is for each step (connect, write, read), in seconds.
[[nodiscard]] Result<HttpResult> http_request(std::string_view method, std::string_view url,
                                              const std::vector<std::pair<std::string, std::string>>& headers,
                                              std::string_view body, const std::string& ca_file, int timeout_seconds);

// A JSON value. Enough for the answers of an ACME server.
class Json {
 public:
  using Array = std::vector<Json>;
  using Object = std::vector<std::pair<std::string, Json>>;

  Json() = default;
  [[nodiscard]] static Result<Json> parse(std::string_view text);

  [[nodiscard]] bool is_object() const noexcept { return std::holds_alternative<Object>(data_); }
  [[nodiscard]] bool is_array() const noexcept { return std::holds_alternative<Array>(data_); }
  [[nodiscard]] const Json* find(std::string_view key) const noexcept;
  [[nodiscard]] std::string_view str(std::string_view key) const noexcept;  // "" if missing or not a string
  [[nodiscard]] std::string_view as_string() const noexcept;
  [[nodiscard]] const Array& array() const noexcept;

 private:
  friend class JsonParser;
  std::variant<std::monostate, bool, double, std::string, Array, Object> data_;
};

}  // namespace campfire::net::front
