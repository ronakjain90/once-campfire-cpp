// Rack::Mime and the compressible types of ActionDispatch::Static (Rust: crates/assets/src/serve.rs).
#pragma once

#include <optional>
#include <string_view>

namespace campfire::assets {

// Rack::Mime.mime_type(ext, nil) for the extensions of a Campfire deploy and the other common web
// ones. The lookup ignores case. `extension` includes the dot.
[[nodiscard]] std::optional<std::string_view> mime_type(std::string_view extension) noexcept;

// File.extname on bytes.
[[nodiscard]] std::string_view file_extname(std::string_view path) noexcept;

// ActionDispatch::Static compressible_content_types:
// /\A(?:text\/|application\/javascript|image\/svg\+xml)/
[[nodiscard]] bool compressible(std::string_view content_type) noexcept;

}  // namespace campfire::assets
