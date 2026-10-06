// The part of the Room model that the message area needs. Rails: app/models/room.rb (users, memberships, direct?),
// app/models/user.rb (rooms, reachable_messages). Rust: crates/db/src/models/room.rs.
// The full room model belongs to the rooms area (A2). This file only reads rows.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/arena.hpp"
#include "core/error.hpp"
#include "db/connection.hpp"
#include "models/user.hpp"

namespace campfire::models {

// `Room`, with the STI class as the `type` column holds it.
struct RoomRef {
  std::int64_t id = 0;
  std::string created_at;
  std::int64_t creator_id = 0;
  std::optional<std::string> name;
  std::string type;  // "Rooms::Open", "Rooms::Closed" or "Rooms::Direct"
  std::string updated_at;

  [[nodiscard]] bool direct() const noexcept { return type == "Rooms::Direct"; }
  // `Room.model_name.param_key` of the STI class: the stem of `dom_id(room)`.
  [[nodiscard]] std::string param_key() const;
};

namespace room_refs {

// `Current.user.rooms.find_by(id:)`
[[nodiscard]] Result<std::optional<RoomRef>> find_for_user(db::Connection& conn, Arena& arena, std::int64_t user_id,
                                                           std::int64_t room_id);
// `Room.find_by(id:)`
[[nodiscard]] Result<std::optional<RoomRef>> find(db::Connection& conn, Arena& arena, std::int64_t room_id);
// `room.memberships.pluck(:user_id)`
[[nodiscard]] Result<std::vector<std::int64_t>> member_user_ids(db::Connection& conn, Arena& arena,
                                                                std::int64_t room_id);
// `room.users`
[[nodiscard]] Result<std::vector<User>> users(db::Connection& conn, Arena& arena, std::int64_t room_id);
// `room.users.active_bots`
[[nodiscard]] Result<std::vector<User>> active_bots(db::Connection& conn, Arena& arena, std::int64_t room_id);
// `room.users.where(id: ids)`
[[nodiscard]] Result<std::vector<User>> members_among(db::Connection& conn, Arena& arena, std::int64_t room_id,
                                                      const std::vector<std::int64_t>& user_ids);
// `room.users.active.filtered_by(query).ordered`, or `User.active...` for room_id 0: the autocomplete scope.
[[nodiscard]] Result<std::vector<User>> autocompletable_users(db::Connection& conn, Arena& arena,
                                                              std::optional<std::int64_t> room_id,
                                                              std::optional<std::string_view> query);

}  // namespace room_refs
}  // namespace campfire::models
