// Request view and HTTP types. Rust: crates/kit/src/front/conn.rs (hyper request); Rails: Rack env.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace campfire::net {

enum class Method : std::uint8_t { Get, Head, Post, Put, Patch, Delete, Options, Other };
inline constexpr std::size_t kMethodCount = 8;

[[nodiscard]] Method parse_method(std::string_view text) noexcept;
[[nodiscard]] std::string_view method_name(Method method) noexcept;

// ASCII case-insensitive equality.
[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept;

struct Header {
  std::string_view name;
  std::string_view value;
};

// One parsed request. All views point into the read buffer or the arena of the request. They are
// valid until the response is written.
struct Request {
  Method method = Method::Get;
  std::string_view method_text;
  std::string_view target;  // as on the wire
  std::string_view path;    // without the query, in origin form
  std::string_view query;   // without the "?"
  int minor_version = 1;
  std::span<const Header> headers;
  std::string_view body;
  bool keep_alive = true;
  bool via_front = false;      // came in on HTTP_PORT (the front), not TARGET_PORT
  std::string_view remote_ip;  // text of the client address, canonical form

  // First header with this name (`lower_name` must be lowercase), or an empty view.
  [[nodiscard]] std::string_view header(std::string_view lower_name) const noexcept;
  [[nodiscard]] bool has_header(std::string_view lower_name) const noexcept;
};

}  // namespace campfire::net
