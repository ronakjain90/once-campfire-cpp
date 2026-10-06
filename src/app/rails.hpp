// Headers that Rails middleware adds. Rails: ActionDispatch::RequestId, Rack::Runtime, Rack::Deflater.
#pragma once

#include <string_view>

#include "net/ctx.hpp"

namespace campfire::app {

// Adds "x-request-id" and "x-runtime" at the end of the headers.
void add_rails_tail(net::Ctx& ctx, net::Response& response);

// True if Rack::Deflater would choose gzip for this request.
[[nodiscard]] bool wants_gzip(const net::Request& request) noexcept;

// True if the client refuses both gzip and identity: Rack::Deflater answers 406 (for a response
// that it may compress).
[[nodiscard]] bool refuses_every_encoding(const net::Request& request);
// That 406: "text/plain" with the message, and nothing else (the middleware replaces the response).
[[nodiscard]] net::Response not_acceptable(net::Ctx& ctx);

}  // namespace campfire::app
