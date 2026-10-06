// Ruby's Base64 flavors as Rails uses them (Rust: crates/rails_compat/src/encoding.rs).
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace campfire::compat::base64 {

// Base64.strict_encode64: standard alphabet, padded.
std::string strict_encode(std::string_view data);
// Base64.urlsafe_encode64(data, padding: false).
std::string urlsafe_encode_unpadded(std::string_view data);
// Base64.urlsafe_encode64(data), padded.
std::string urlsafe_encode_padded(std::string_view data);

// Base64.strict_decode64: standard alphabet, canonical padding, no whitespace,
// no non-zero trailing bits.
std::optional<std::string> strict_decode(std::string_view encoded);
// Base64.urlsafe_decode64: pads a short unpadded string, then translates "-_"
// to "+/" and decodes strictly. It accepts either alphabet, even mixed.
std::optional<std::string> urlsafe_decode(std::string_view encoded);

}  // namespace campfire::compat::base64
