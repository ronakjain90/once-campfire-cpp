// The view models of the room forms, the direct room pages and the involvement. Rust: crates/views/src/rooms.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "routes/routes.hpp"
#include "views/users/sidebar.hpp"

namespace campfire::views {

// The class of a room: `Rooms::Open`, `Rooms::Closed` or `Rooms::Direct`.
enum class RoomKind : std::uint8_t { Open, Closed, Direct };

// `model_name.param_key`, the stem of `dom_id(room)`.
[[nodiscard]] inline std::string_view room_param_key(RoomKind kind) {
  switch (kind) {
    case RoomKind::Open: return "rooms_open";
    case RoomKind::Closed: return "rooms_closed";
    case RoomKind::Direct: return "rooms_direct";
  }
  return {};
}

// The room that a form creates or edits. `id` is empty for a new record.
struct FormRoom {
  std::optional<std::int64_t> id;
  std::optional<std::string> name;

  // `form_with model: room`: the action for an open or a closed room.
  [[nodiscard]] std::string action(RoomKind kind) const {
    if (id) return kind == RoomKind::Open ? campfire::routes::rooms_open(*id) : campfire::routes::rooms_closed(*id);
    return kind == RoomKind::Open ? campfire::routes::rooms_opens() : campfire::routes::rooms_closeds();
  }
  [[nodiscard]] std::string_view display_name() const { return name ? std::string_view(*name) : std::string_view{}; }
};

// `rooms/opens/new` and `rooms/opens/edit`.
struct OpenFormView {
  FormRoom room;
  // `Current.user.can_administer?(room)`: administrators, the creator, or a new room.
  bool can_administer = false;
  // `User.active.ordered`.
  std::vector<UserSummary> users;
};

// `rooms/closeds/new` and `rooms/closeds/edit`.
struct ClosedFormView {
  FormRoom room;
  bool can_administer = false;
  std::int64_t current_user_id = 0;
  std::vector<UserSummary> selected_users;
  std::vector<UserSummary> unselected_users;
};

// `rooms/directs/edit`.
struct DirectEditView {
  std::int64_t room_id = 0;
  // `room_display_name(@room)` for `Current.user`.
  std::string display_name;
  // `@room.users.many? ? @room.users.without(Current.user) : @room.users`.
  std::vector<UserSummary> users;
};

// `rooms/involvements/show`.
struct InvolvementView {
  std::int64_t room_id = 0;
  RoomKind kind = RoomKind::Open;
  // "mentions", "everything", "nothing" or "invisible".
  std::string involvement;
};

}  // namespace campfire::views
