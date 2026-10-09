// Rack::MethodOverride (Rails: the middleware before the router; Rust: method_override in crates/kit/src/adapter.rs).
#pragma once

#include <optional>
#include <string_view>

namespace campfire::req {

// The method that a POST asks for: the `_method` param of a form body, or else the X-HTTP-Method-Override
// header. Returns the method in upper case ("PATCH"), or nothing if the request asks for no method of
// Rack::MethodOverride::HTTP_METHODS. Only a form body (no content type, urlencoded or multipart) can hold
// `_method`. A body that the parser rejects gives no `_method`: the action then reports the error of the body.
// The caller calls this for a POST only.
[[nodiscard]] std::optional<std::string_view> method_override(std::optional<std::string_view> content_type,
                                                              std::string_view body,
                                                              std::optional<std::string_view> override_header);

}  // namespace campfire::req
