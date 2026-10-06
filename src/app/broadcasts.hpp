// The broadcasts of the message area. Rails: Turbo::Streams::Broadcasts (broadcast_*_to), app/models/message/
// broadcasts.rb, app/controllers/messages/boosts_controller.rb. Rust: crates/campfire/src/channels/broadcasts.rs and
// crates/cable/src/turbo.rs. The frame bytes are the bytes of the Rust port.
//
// Each function renders nothing: the caller gives the HTML. A broadcast goes to the hub of the app. With no hub it does
// nothing.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "app/app.hpp"
#include "models/room_ref.hpp"

namespace campfire::app::broadcasts {

enum class Action : std::uint8_t { Append, Prepend, Replace, Update, Remove, Before, After, Refresh };

struct Attribute {
  std::string_view name;
  std::optional<std::string_view> value;  // nothing: the attribute is left out
};

// `turbo_stream_action_tag(action, target:, template:, **attributes)`: the extra attributes come first, then `action`,
// then `target`. Remove and refresh have no template.
[[nodiscard]] std::string action_tag(Action action, std::string_view target, std::optional<std::string_view> html,
                                     std::span<const Attribute> attributes = {});

// `<room gid param>:messages`: `turbo_stream_from @room, :messages`.
[[nodiscard]] std::string room_messages_stream(const models::RoomRef& room);
// `UnreadRoomsChannel.stream_name_for(user_id)`
[[nodiscard]] std::string unread_rooms_stream(std::int64_t user_id);

// `dom_id(room, prefix)` for the STI class of the room.
[[nodiscard]] std::string room_dom_id(const models::RoomRef& room, std::string_view prefix);

// `broadcast_stream_to`: sends the tag on the stream of the room messages.
std::size_t to_room_messages(const App& app, const models::RoomRef& room, std::string_view tag);

// `message.broadcast_append_to room, :messages, target: [room, :messages]`
void message_append(const App& app, const models::RoomRef& room, std::string_view message_html);
// `broadcast_unread_room`: `{ roomId: }` to the unread stream of each member.
void unread_room(const App& app, const models::RoomRef& room, std::span<const std::int64_t> member_ids);
// `message.broadcast_remove_to room, :messages`
void message_remove(const App& app, const models::RoomRef& room, std::string_view client_message_id);
// `broadcast_replace_to @room, :messages, target: [message, :presentation], ..., attributes: { maintain_scroll: true }`
void message_replace_presentation(const App& app, const models::RoomRef& room, std::string_view client_message_id,
                                  std::string_view presentation_html);
// `@boost.broadcast_append_to room, :messages, target: "boosts_message_<client id>", ...`
void boost_append(const App& app, const models::RoomRef& room, std::string_view client_message_id,
                  std::string_view boost_html);
// `@boost.broadcast_remove_to room, :messages`: the target is `dom_id(boost)`.
void boost_remove(const App& app, const models::RoomRef& room, std::int64_t boost_id);

}  // namespace campfire::app::broadcasts
