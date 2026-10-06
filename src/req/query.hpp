// Rack and Rails query and form parsing (Rack::QueryParser, ActionDispatch::ParamBuilder;
// Rust: crates/kit/src/params.rs). Pure functions over bytes and a memory resource.
#pragma once

#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "req/param.hpp"

namespace campfire::req {

inline constexpr std::size_t kDepthLimit = 100;              // ParamBuilder.default depth limit
inline constexpr std::size_t kFormBytesizeLimit = 4U << 20;  // Rack::QueryParser bytesize limit
inline constexpr std::size_t kFormParamsLimit = 4096;        // Rack::QueryParser params limit

// A pair after %-decoding, before the UTF-8 check (Rails checks it in the builder, so a pair
// with an empty top-level key is skipped without an error).
struct RawPair {
  std::string key;
  bool has_value = false;
  std::string value;                   // when `has_value` and `file` is null
  std::shared_ptr<UploadedFile> file;  // a multipart file part
};

// URI.decode_www_form_component: "+" is a space, %XX is a byte. A bad "%" is an error.
[[nodiscard]] ParamResult<std::string> decode_www_form_component(std::string_view s);

// ActionDispatch::QueryParser.each_pair. No size limit, as Rails has none for the query string.
[[nodiscard]] ParamResult<std::vector<RawPair>> query_pairs(std::string_view qs);
// Rack::Request#form_pairs for an urlencoded body, with the Rack limits.
[[nodiscard]] ParamResult<std::vector<RawPair>> form_pairs(std::string_view body);
// ParamBuilder.from_pairs.
[[nodiscard]] ParamResult<ParamMap> from_pairs(std::vector<RawPair> pairs, std::pmr::memory_resource* mr);
// ParamBuilder.from_query_string.
[[nodiscard]] ParamResult<ParamMap> from_query_string(std::string_view qs, std::pmr::memory_resource* mr);
// An urlencoded body to params.
[[nodiscard]] ParamResult<ParamMap> from_form_body(std::string_view body, std::pmr::memory_resource* mr);
// A JSON body to params. A document that is not an object becomes { "_json" => data }.
[[nodiscard]] ParamResult<ParamMap> from_json_body(std::string_view body, std::pmr::memory_resource* mr);

}  // namespace campfire::req
