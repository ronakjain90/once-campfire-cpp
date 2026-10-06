// ActionDispatch::Static over Rack::Files for the public directory and the precompiled assets
// (Rails: actionpack static.rb; Rust: crates/assets/src/serve.rs).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "assets/assets.hpp"

namespace campfire::assets {

// The parts of a request that ActionDispatch::Static reads.
struct StaticRequest {
  std::string_view method;
  std::string_view path;  // the raw path, percent-encoded, with no query string
  std::optional<std::string_view> range = std::nullopt;
  std::optional<std::string_view> if_modified_since = std::nullopt;
};

struct StaticResponse {
  int status = 200;
  // In the order that Rack builds them. The names have the case that Rack gives them.
  std::vector<std::pair<std::string_view, std::string>> headers;
  // The file of the response. The server may send a gzip or zstd body of it for a 200 response
  // (see `choose_body`). Null for a 304 or 416 response.
  std::optional<StaticFile> file;

  [[nodiscard]] std::string_view body() const noexcept { return owned_ ? std::string_view(*owned_) : borrowed_; }
  [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const;

  std::string_view borrowed_;         // a slice of the file
  std::optional<std::string> owned_;  // a multipart body or a message
};

// A response for a GET or HEAD request that a file matches. Null if the app must answer.
[[nodiscard]] std::optional<StaticResponse> serve(const StaticRequest& request);

}  // namespace campfire::assets
