// A whole request body to params (Rails: ActionDispatch::Request#POST and #request_parameters,
// Rack::Request#form_pairs; Rust: crates/kit/src/body.rs). For a body that is in memory.
// T5 can feed `MultipartParser` chunk by chunk when it streams.
#pragma once

#include <expected>
#include <filesystem>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "req/multipart.hpp"
#include "req/param.hpp"

namespace campfire::req {

// `Rack::MediaType.type`: the "type/subtype" part of Content-Type in lower case.
[[nodiscard]] std::optional<std::string> media_type(std::optional<std::string_view> content_type);

struct ParsedBody {
  std::string raw;  // request.raw_post: empty for multipart
  ParamResult<ParamMap> params;
};

struct BodyTooLarge {};

// `original_method` is the method on the wire: Rack treats a POST with no content type as a form.
// `limit` is the configured body limit. A body over a limit gives BodyTooLarge (413).
[[nodiscard]] std::expected<ParsedBody, BodyTooLarge> parse_body(std::string_view original_method,
                                                                 std::optional<std::string_view> content_type,
                                                                 std::string_view body,
                                                                 const std::filesystem::path& tmp_dir,
                                                                 std::pmr::memory_resource* mr,
                                                                 std::optional<std::size_t> limit = std::nullopt);

}  // namespace campfire::req
