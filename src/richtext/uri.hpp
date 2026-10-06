// The part of Ruby's URI.parse (uri 1.1, RFC 3986 parser) that the opengraph URL checks and the tweet
// URL normalization use, including which inputs raise which error. Rust: crates/richtext/src/uri.rs
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace campfire::richtext {

enum class UriError : std::uint8_t {
  InvalidUri,        // URI::InvalidURIError, which callers rescue
  InvalidComponent,  // URI::InvalidComponentError (from URI::MailTo), which nothing rescues
};

struct Uri {
  std::optional<std::string> scheme;
  std::optional<std::string> userinfo;
  std::optional<std::string> host;
  std::optional<std::uint64_t> port;
  std::optional<std::string> path;
  std::optional<std::string> opaque;
  std::optional<std::string> query;
  std::optional<std::string> fragment;

  // `uri.is_a?(URI::HTTP)`, which includes URI::HTTPS.
  [[nodiscard]] bool is_http() const;
  // URI::Generic#to_s
  [[nodiscard]] std::string to_s() const;
};

// `URI.parse(value)`
[[nodiscard]] std::expected<Uri, UriError> parse_uri(std::string_view value);

}  // namespace campfire::richtext
