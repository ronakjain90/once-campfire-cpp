// What a bot webhook does with the reply of the bot. Rails: app/models/webhook.rb (`extract_text_from`,
// `extract_attachment_from`), Action Dispatch `Mime::Type.lookup`. Rust: crates/campfire/src/integrations/webhook.rs.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace campfire::jobs::webhook {

// `Webhook::ENDPOINT_TIMEOUT`: the connect and each read.
inline constexpr std::chrono::seconds kEndpointTimeout{7};
// The most that a delivery may take, the connect and the reply included. Rails has no such limit.
inline constexpr std::chrono::seconds kDeliveryDeadline{60};
// The largest reply (after decompression). A larger one fails the delivery. Rails reads any size into memory.
inline constexpr std::size_t kMaxReplySize = std::size_t{100} * 1024 * 1024;

struct Attachment {
  std::string data;
  std::string filename;      // `"attachment.#{mime_type.symbol}"`: "attachment." for an unregistered type
  std::string content_type;  // `mime_type.to_s`: the registered type, which for a synonym differs from what came
};

// What the bot says in the room.
struct Reply {
  enum class Kind : std::uint8_t {
    None,
    Text,        // `room.messages.create!(body: text, creator: bot)`
    Attachment,  // `room.messages.create_with_attachment!(attachment: blob, creator: bot)`
  };
  Kind kind = Kind::None;
  std::string text;
  jobs::webhook::Attachment attachment;
};

// `Mime::Type::InvalidMimeType`
struct InvalidMimeType {
  std::string content_type;
};

// `extract_text_from`, else `extract_attachment_from`. `content_type` is the media type of the response (the part
// before
// ";"), if it has one.
[[nodiscard]] std::expected<Reply, InvalidMimeType> reply_from(int status,
                                                               const std::optional<std::string>& content_type,
                                                               std::string_view body);

// The text that stands for a timeout: "Failed to respond within N seconds".
[[nodiscard]] Reply timed_out(std::chrono::seconds after);

// `Mime::Type.lookup(string)`: the symbol (empty for a type that is not registered) and the type as a string.
struct MimeLookup {
  std::string symbol;
  std::string content_type;
};
[[nodiscard]] std::expected<MimeLookup, InvalidMimeType> mime_lookup(std::string_view string);

}  // namespace campfire::jobs::webhook
