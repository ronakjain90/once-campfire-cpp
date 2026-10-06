// What the message views read off a message. Rails: the locals of app/views/messages/_message.html.erb and the records
// behind them. Rust: crates/views/src/messages.rs (MessageView, BoostView, UserView).
// The views never read the database: the presenter fills these structs first.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/timestamp.hpp"

namespace campfire::views::messages {

// What the message views show of a user: `avatar_tag` and the author heading.
struct UserView {
  std::int64_t id = 0;
  std::string name;
  std::string title;       // `User#title`: name and bio joined by " – "
  std::string avatar_url;  // `fresh_user_avatar_path(user)`
};

enum class RoomKind : std::uint8_t { Open, Closed, Direct };

// `Rooms::Open.model_name.param_key`, the stem of `dom_id(room)`.
[[nodiscard]] constexpr std::string_view room_param_key(RoomKind kind) noexcept {
  switch (kind) {
    case RoomKind::Open: return "rooms_open";
    case RoomKind::Closed: return "rooms_closed";
    case RoomKind::Direct: return "rooms_direct";
  }
  return "";
}

struct SoundImage {
  std::string src;  // `image_path(image.asset_path)`
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};
// A `/play <name>` message's `Sound`.
struct SoundView {
  std::string url;  // `asset_path(sound.asset_path)`
  std::optional<SoundImage> image;
  std::optional<std::string> text;
};

// `attachment.metadata[:width]`: an Integer for an image, a Float for a video.
struct RubyNumber {
  bool is_float = false;
  double value = 0;
};

struct AttachmentPreview {
  enum class Kind : std::uint8_t {
    Video,  // `attachment.video?`: the poster from `attachment.preview(format: :webp, resize_to_limit: ...)`
    Image,  // `polymorphic_url(attachment.representation(:thumb), only_path: true)`
    File,   // neither previewable nor variable: a download link
  };
  Kind kind = Kind::File;
  std::string url;  // the poster or the thumbnail
};

struct AttachmentView {
  std::string filename;       // `attachment.filename.to_s`
  std::string blob_path;      // `rails_blob_path(attachment)`
  std::string download_path;  // `rails_blob_path(attachment, disposition: "attachment")`
  AttachmentPreview preview;
  std::optional<RubyNumber> width;
  std::optional<RubyNumber> height;
};

struct BoostView {
  std::int64_t id = 0;
  std::string updated_at;  // for the fragment cache key
  std::int64_t message_id = 0;
  std::string content;
  bool all_emoji = false;  // `boost.content.all_emoji?`
  UserView booster;
};

// `message.content_type` with what each presentation needs.
struct TextContent {
  std::string html;  // the presentation filters' output after `auto_link`
};
struct UnrenderableContent {};  // `message_tag` rescued an exception: messages/_unrenderable
using MessageContent = std::variant<TextContent, SoundView, AttachmentView, UnrenderableContent>;

// A message as `messages/_message` shows it.
struct MessageView {
  std::int64_t id = 0;
  std::string client_message_id;  // `Message#to_key`: every `dom_id(message)` uses it
  std::int64_t room_id = 0;
  std::string room_name;  // `room_display_name(message.room, for_user: nil)`
  UserView creator;
  std::string created_at;  // the database text
  std::string updated_at;
  bool all_emoji = false;  // `message.plain_text_body.all_emoji?`
  MessageContent content;
  std::vector<BoostView> boosts;  // `message.boosts.ordered`

  [[nodiscard]] const AttachmentView* attachment() const { return std::get_if<AttachmentView>(&content); }
  [[nodiscard]] bool is_unrenderable() const { return std::holds_alternative<UnrenderableContent>(content); }
  // `dom_id(message, prefix)`
  [[nodiscard]] std::string dom_id(std::string_view prefix = {}) const {
    std::string result;
    if (!prefix.empty()) {
      result.append(prefix);
      result.push_back('_');
    }
    result += "message_";
    result += client_message_id;
    return result;
  }
};

}  // namespace campfire::views::messages
