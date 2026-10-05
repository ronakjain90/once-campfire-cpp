// The Action Cable wire format. Rails: ActionCable::INTERNAL, Connection::Base, Channel::Base.
// Rust: crates/cable/src/protocol.rs and naming.rs. The functions return the exact JSON bytes
// (key order and spacing) that Rails sends.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace campfire::cable::protocol {

inline constexpr std::string_view kDefaultMountPath = "/cable";
// ActionCable::Server::Connections::BEAT_INTERVAL, in seconds.
inline constexpr int kBeatIntervalSeconds = 3;

// Limits (Rust README, "Cable limits").
inline constexpr std::size_t kMaxSubscriptions = 64;
inline constexpr std::size_t kMaxIdentifierBytes = 4096;
inline constexpr int kWriteStallSeconds = 30;

enum class DisconnectReason : std::uint8_t { Unauthorized, InvalidRequest, ServerRestart, Remote };
[[nodiscard]] std::string_view to_string(DisconnectReason reason);

[[nodiscard]] std::string welcome();
[[nodiscard]] std::string ping(std::int64_t unix_seconds);
// `reconnect_json` is a JSON value, already encoded. Rails copies it from the remote message.
[[nodiscard]] std::string disconnect(std::optional<DisconnectReason> reason, std::string_view reconnect_json);
[[nodiscard]] std::string confirmation(std::string_view identifier);
[[nodiscard]] std::string rejection(std::string_view identifier);
// Both arguments are already JSON encoded.
[[nodiscard]] std::string message(std::string_view encoded_identifier, std::string_view encoded_message);

// ActionCable::Connection::InternalChannel: "action_cable/<connection identifier>".
[[nodiscard]] std::string internal_channel(std::string_view connection_identifier);

// ActionCable::Channel::Naming.channel_name.
[[nodiscard]] std::string channel_name(std::string_view class_name);
// Channel::Broadcasting.broadcasting_for: the channel name and the parts, joined with ":".
[[nodiscard]] std::string broadcasting_for(std::string_view class_name, std::span<const std::string_view> parts);
// Turbo::Streams::StreamName#stream_name_from.
[[nodiscard]] std::string stream_name_from(std::span<const std::string_view> parts);

}  // namespace campfire::cable::protocol
