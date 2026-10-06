// `Current.user.rooms` as the welcome page and `last_room_visited` use it. Rails: app/models/user.rb (has_many :rooms,
// through: :memberships), room.rb (`original`). Rust: Room::for_user, find_for_user, original_for_user.
// The full room model is the task of the rooms area (A2).
#pragma once

#include <cstdint>
#include <optional>

#include "core/arena.hpp"
#include "core/error.hpp"
#include "db/connection.hpp"

namespace campfire::models::user_rooms {

// `user.rooms.any?`
[[nodiscard]] Result<bool> any(db::Connection& conn, Arena& arena, std::int64_t user_id);
// `user.rooms.find_by(id:)`: the id of the room, if the user is a member.
[[nodiscard]] Result<std::optional<std::int64_t>> find(db::Connection& conn, Arena& arena, std::int64_t user_id,
                                                       std::int64_t room_id);
// `user.rooms.original`: the oldest room of the user.
[[nodiscard]] Result<std::optional<std::int64_t>> original(db::Connection& conn, Arena& arena, std::int64_t user_id);

}  // namespace campfire::models::user_rooms
