// Gzip for dynamic bodies. Rails: Rack::Deflater (zlib, default level). Rust: crates/kit/src/deflater.rs.
#pragma once

#include <string>
#include <string_view>

namespace campfire::app {

// A gzip member of `bytes` (libdeflate, level 6). The mtime field is 0.
[[nodiscard]] std::string gzip_compress(std::string_view bytes);

}  // namespace campfire::app
