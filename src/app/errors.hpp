// What an error looks like on the wire in production. Rails: ActionDispatch::PublicExceptions and
// ShowExceptions. Rust: crates/kit/src/exceptions.rs.
#pragma once

#include <string_view>

#include "net/ctx.hpp"
#include "req/format.hpp"

namespace campfire::app {

// `Rack::Utils::HTTP_STATUS_CODES` (422 is "Unprocessable Content", 413 is "Content Too Large").
[[nodiscard]] std::string_view rack_reason(int status) noexcept;

// An error response. JSON, XML and YAML get a `{ status:, error: }` hash; other formats get the page
// `public/<status>.html` (404, 422, 500, 502) or an empty body. A HEAD request gets an empty body.
// The type has `charset=UTF-8`, and the response has an explicit content-length (as Rust does:
// the gzip step then skips an empty body). It has no x-request-id: `finish_response` adds it.
[[nodiscard]] net::Response render_error(net::Ctx& ctx, int status, req::Format format, bool head);

}  // namespace campfire::app
