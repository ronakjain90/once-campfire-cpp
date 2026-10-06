// ActionDispatch::Static and Rack::Deflater for the files of the asset table.
// Rust: crates/campfire/src/app.rs (public_files, static_response), crates/kit/src/deflater.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "net/ctx.hpp"
#include "net/response.hpp"

namespace campfire::net::front {

// The response for a GET or HEAD request that a file of the table matches, or nothing. It has
// the headers and the encoding that the Rust app sends before the front adds its own headers.
// Rack::Deflater is part of it: gzip for a client that accepts it, a 406 for one that refuses
// every encoding.
[[nodiscard]] std::optional<Response> serve_static(Ctx& ctx);

// What `Rack::Utils.select_best_encoding(["gzip", "identity"], ...)` chooses for an
// "Accept-Encoding" value. `None` is a client that refuses both: Rack::Deflater answers 406.
enum class DeflaterChoice : std::uint8_t { Gzip, Identity, None };
[[nodiscard]] DeflaterChoice choose_deflater_encoding(std::string_view accept_encoding);

}  // namespace campfire::net::front
