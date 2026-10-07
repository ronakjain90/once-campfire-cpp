// Helpers of the message views. Rails: app/helpers/users_helper.rb (avatar_tag), app/helpers/rooms/refreshes_helper.rb
// is not used here; `mention_prompt_tag` is in app/helpers/messages_helper.rb. Rust: crates/views/src/helpers/users.rs,
// crates/views/src/rooms.rs (mention_prompt_src).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "views/context.hpp"
#include "views/helpers/forms.hpp"
#include "views/helpers/tag.hpp"
#include "views/messages/types.hpp"

namespace campfire::views::messages {

// `avatar_tag(user, **options)`: the options go to the image.
void avatar_tag(Out& out, const ViewContext& ctx, std::int64_t user_id, std::string_view title,
                std::string_view avatar_url, helpers::Attrs options = {});

// `mention_prompt_tag(room)`: the `src` is `autocompletable_users_path(room_id: room.id)`.
[[nodiscard]] std::string mention_prompt_src(std::int64_t room_id);

}  // namespace campfire::views::messages

namespace campfire::views::messages {

// `EmojiHelper::REACTIONS`.
struct Reaction {
  std::string_view character;
  std::string_view title;
};
inline constexpr std::array<Reaction, 8> kReactions = {{
    {"\xF0\x9F\x91\x8D", "Thumbs up"},
    {"\xF0\x9F\x91\x8F", "Clapping"},
    {"\xF0\x9F\x91\x8B", "Waving hand"},
    {"\xF0\x9F\x92\xAA", "Muscle"},
    {"\xE2\x9D\xA4\xEF\xB8\x8F", "Red heart"},
    {"\xF0\x9F\x98\x82", "Face with tears of joy"},
    {"\xF0\x9F\x8E\x89", "Party popper"},
    {"\xF0\x9F\x94\xA5", "Fire"},
}};

// The key of the fragment cache for `cache [ message, "presentation-v3" ]` and for `cache boost`: the id and the
// `updated_at` of the record, as `cache_key_with_version` does. The version of the template is in the text.
[[nodiscard]] std::string message_fragment_key(std::int64_t id, std::string_view updated_at);
[[nodiscard]] std::string boost_fragment_key(std::int64_t id, std::string_view updated_at);

// `message.created_at.iso8601`, `to_fs(:epoch)` of `created_at` and of `updated_at`.
[[nodiscard]] std::string created_at_iso(const MessageView& message);
[[nodiscard]] std::int64_t created_at_epoch(const MessageView& message);
[[nodiscard]] std::int64_t updated_at_epoch(const MessageView& message);

// `room_at_message_path`, `edit_room_message_path`, `message_boosts_path`, `new_message_boost_path`.
[[nodiscard]] std::string at_path(const MessageView& message);
[[nodiscard]] std::string edit_path(const MessageView& message);
[[nodiscard]] std::string boosts_path(const MessageView& message);
[[nodiscard]] std::string new_boost_path(const MessageView& message);
// `message_boost_path(boost.message, boost)`.
[[nodiscard]] std::string boost_path(const BoostView& boost);

// `message_presentation(message)`: the attachment, the sound or the text of the message. Writes it to `out`.
void message_presentation(Out& out, const ViewContext& ctx, const MessageView& message);

}  // namespace campfire::views::messages

namespace campfire::views::messages {

// `message_attachment_presentation(message)`: `Messages::AttachmentPresentation#render`.
void attachment_presentation(Out& out, const ViewContext& ctx, const AttachmentView& attachment);

}  // namespace campfire::views::messages
