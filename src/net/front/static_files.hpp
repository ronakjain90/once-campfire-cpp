// ActionDispatch::Static and Rack::Deflater for the files of the asset table.
// Rust: crates/campfire/src/app.rs (public_files, static_response), crates/kit/src/deflater.rs.
#pragma once

#include <optional>

#include "net/ctx.hpp"
#include "net/response.hpp"

namespace campfire::net::front {

// The response for a GET or HEAD request that a file of the table matches, or nothing. It has
// the headers and the encoding that the Rust app sends before the front adds its own headers.
// Rack::Deflater is part of it: gzip for a client that accepts it, a 406 for one that refuses
// every encoding.
[[nodiscard]] std::optional<Response> serve_static(Ctx& ctx);

}  // namespace campfire::net::front
