// The view models of the room page, the refresh and the messages page. Rails: the locals and instance variables of
// app/views/rooms/show.html.erb, rooms/refreshes/show.turbo_stream.erb and messages/index.html.erb. Rust:
// crates/views/src/rooms.rs (ShowView, RefreshView), crates/views/src/messages.rs (MessageItem).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "routes/routes.hpp"
#include "views/helpers/turbo.hpp"
#include "views/messages/types.hpp"
#include "views/rooms/forms.hpp"

namespace campfire::views::rooms {

// A persisted room, as the room page shows it.
struct RoomView {
  std::int64_t id = 0;
  RoomKind kind = RoomKind::Open;
  // `room_display_name(room)` for `Current.user`.
  std::string display_name;

  [[nodiscard]] bool is_direct() const { return kind == RoomKind::Direct; }
  // `dom_id(room, prefix)`
  [[nodiscard]] std::string dom_id(std::string_view prefix) const {
    return helpers::dom_id(room_param_key(kind), std::to_string(id), prefix);
  }
  // `edit_polymorphic_path(room)`
  [[nodiscard]] std::string edit_path() const {
    switch (kind) {
      case RoomKind::Open: return campfire::routes::edit_rooms_open(id);
      case RoomKind::Closed: return campfire::routes::edit_rooms_closed(id);
      case RoomKind::Direct: return campfire::routes::edit_rooms_direct(id);
    }
    return {};
  }
  // "Ping" for a direct room, "room" for the others.
  [[nodiscard]] std::string_view noun() const { return is_direct() ? "Ping" : "room"; }
};

// `rooms/show`.
struct ShowView {
  RoomView room;
  std::string loaded_at;    // `room.updated_at.to_fs(:epoch)`
  messages::UserView user;  // `Current.user`, for the client template
  std::vector<messages::MessageItem> items;
  bool invitation = false;  // `@room == Room.original && !@room.messages.paged?`
  std::string join_code;
  std::string messages_stream_name;  // `Turbo::StreamsChannel.signed_stream_name([ room, :messages ])`
};

// `rooms/refreshes/show.turbo_stream`.
struct RefreshView {
  std::int64_t room_id = 0;
  RoomKind kind = RoomKind::Open;
  std::vector<messages::MessageItem> new_messages;
  std::vector<messages::MessageItem> updated_messages;
};

}  // namespace campfire::views::rooms
