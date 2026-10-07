// The Turbo Stream broadcasts of the room area (A2) and of the message area (A3). Rails: Turbo::Streams::Broadcasts
// (broadcast_*_to), app/models/message/ broadcasts.rb, app/controllers/messages/boosts_controller.rb. Rust:
// crates/campfire/src/channels/broadcasts.rs and crates/cable/src/turbo.rs. The frame bytes are the bytes of the Rust
// port.
//
// Rails of the room area: `broadcast_*_to` of the room controllers. Rust: crates/campfire/src/channels/broadcasts.rs.
//
// Each function renders nothing: the caller gives the HTML. A broadcast goes to the hub of the app. With no hub, it
// goes to the `turbo_broadcast` function of the app (a frame as a string). With neither, it does nothing.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "app/app.hpp"
#include "models/room.hpp"
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
[[nodiscard]] std::string room_dom_id(const models::Room& room, std::string_view prefix);

// The stream name of the streamables: the parts joined with ":", blank ones dropped.
[[nodiscard]] std::string stream_name(std::span<const std::string_view> streamables);
// `[ user, :rooms ]` as the streamables of a user: the GlobalID param and "rooms".
[[nodiscard]] std::string user_rooms_stream(std::int64_t user_id);

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

// The room controllers.
// `broadcast_remove_to :rooms, target: [ @room, :list ]` (RoomsController#destroy).
void room_remove(const App& app, const models::Room& room);
// `broadcast_prepend_to :rooms, target: :shared_rooms` (Rooms::OpensController#create).
void open_room_create(const App& app, std::string_view shared_html);
// `broadcast_replace_to :rooms, target: [ @room, :list ]` (Rooms::OpensController#update).
void open_room_update(const App& app, const models::Room& room, std::string_view shared_html);
// `broadcast_prepend_to user, :rooms, target: :shared_rooms` for each member (Rooms::ClosedsController#create).
void closed_room_create(const App& app, std::span<const std::int64_t> member_ids, std::string_view shared_html);
// `broadcast_replace_to user, :rooms, target: [ @room, :list ]` for each member (Rooms::ClosedsController#update).
void closed_room_update(const App& app, const models::Room& room, std::span<const std::int64_t> member_ids,
                        std::string_view shared_html);
// `membership.broadcast_prepend_to membership.user, :rooms, target: :direct_rooms` (Rooms::DirectsController#create).
void direct_room_create(const App& app, std::int64_t user_id, std::string_view direct_html);
// `broadcast_visibility_changes` of Rooms::InvolvementsController, for a shared room.
void involvement_remove(const App& app, const models::Room& room, std::int64_t user_id);
void involvement_prepend(const App& app, std::int64_t user_id, std::string_view shared_html);

}  // namespace campfire::app::broadcasts
