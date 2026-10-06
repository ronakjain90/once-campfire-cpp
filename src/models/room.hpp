// Rails: app/models/room.rb, app/models/rooms/*.rb. Rust: crates/db/src/models/room.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/arena.hpp"
#include "core/error.hpp"
#include "db/connection.hpp"
#include "db/schema.gen.hpp"

namespace campfire::models {

// The room types: the `type` column of the single table inheritance.
inline constexpr std::string_view kRoomOpen = "Rooms::Open";
inline constexpr std::string_view kRoomClosed = "Rooms::Closed";
inline constexpr std::string_view kRoomDirect = "Rooms::Direct";

// A room row that owns its text.
struct Room {
  std::int64_t id = 0;
  std::string created_at;
  std::int64_t creator_id = 0;
  std::optional<std::string> name;
  std::string type;
  std::string updated_at;

  [[nodiscard]] static Room from_row(const db::schema::RoomRow& row);
  [[nodiscard]] bool is_open() const noexcept { return type == kRoomOpen; }
  [[nodiscard]] bool is_closed() const noexcept { return type == kRoomClosed; }
  [[nodiscard]] bool is_direct() const noexcept { return type == kRoomDirect; }
};

// The room types that an action can reach (`room_scope` of the controllers).
enum class RoomScope : std::uint8_t {
  All,             // `Current.user.rooms`
  WithoutDirects,  // `Current.user.rooms.without_directs`
  Directs,         // `Current.user.rooms.directs`
};

namespace rooms {

// `Current.user.rooms.last`: the room with the highest id.
[[nodiscard]] Result<std::optional<Room>> last_of_user(db::Connection& conn, Arena& arena, std::int64_t user_id);
// `Current.user.rooms.original`: the oldest room.
[[nodiscard]] Result<std::optional<Room>> original_of_user(db::Connection& conn, Arena& arena, std::int64_t user_id);
// `scope.find_by(id:)` on the rooms of a user.
[[nodiscard]] Result<std::optional<Room>> find_for_user(db::Connection& conn, Arena& arena, std::int64_t user_id,
                                                        RoomScope scope, std::int64_t room_id);

}  // namespace rooms
}  // namespace campfire::models
