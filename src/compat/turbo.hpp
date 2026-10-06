// Turbo::StreamsChannel.signed_stream_name / verified_stream_name (Rust: turbo.rs).
// The verifier is MessageVerifier(generate_key("turbo/signed_stream_verifier_key"),
// digest: SHA256, serializer: JSON): strict Base64 of the JSON-dumped name, no envelope.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "compat/secrets.hpp"

namespace campfire::compat::turbo {

// `streamables` are resolved stream name parts joined with ":": a record is its GID param
// (GlobalId::to_param), a symbol or string is itself.
std::string signed_stream_name(const Secrets& secrets, std::span<const std::string_view> streamables);

// The stream name, or nullopt if the signature does not check out. A validly signed number
// is returned as its to_s, as Rails does.
std::optional<std::string> verified_stream_name(const Secrets& secrets, std::string_view signed_name);

}  // namespace campfire::compat::turbo
