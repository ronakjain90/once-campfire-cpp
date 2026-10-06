// Rails: app/models/room.rb, app/models/rooms/*.rb. Rust: crates/db/src/models/room.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/arena.hpp"
#include "core/error.hpp"
#include "db/connection.hpp"
#include "db/database.hpp"
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

// `Room.find_by(id:)`.
[[nodiscard]] Result<std::optional<Room>> find_by_id(db::Connection& conn, Arena& arena, std::int64_t room_id);
// `Current.user.rooms.last`: the room with the highest id.
[[nodiscard]] Result<std::optional<Room>> last_of_user(db::Connection& conn, Arena& arena, std::int64_t user_id);
// `Room.original`: the oldest room of the account.
[[nodiscard]] Result<std::optional<Room>> original(db::Connection& conn, Arena& arena);
// `Current.user.rooms.original`: the oldest room.
[[nodiscard]] Result<std::optional<Room>> original_of_user(db::Connection& conn, Arena& arena, std::int64_t user_id);
// `scope.find_by(id:)` on the rooms of a user.
[[nodiscard]] Result<std::optional<Room>> find_for_user(db::Connection& conn, Arena& arena, std::int64_t user_id,
                                                        RoomScope scope, std::int64_t room_id);

// `Room.create!(name:, creator:)` of one room class. An open room grants itself to every active user
// (`Rooms::Open#grant_access_to_all_users`, here in the same transaction).
[[nodiscard]] Result<Room> create(db::Tx& tx, std::string_view type, std::optional<std::string_view> name,
                                  std::int64_t creator_id);
// `Room.create_for(attributes, users:)`.
[[nodiscard]] Result<Room> create_for(db::Tx& tx, std::string_view type, std::optional<std::string_view> name,
                                      std::int64_t creator_id, std::span<const std::int64_t> user_ids);
// `Rooms::Direct.find_or_create_for(users)`: the direct room whose members are exactly `user_ids`.
[[nodiscard]] Result<Room> find_or_create_direct_for(db::Tx& tx, std::span<const std::int64_t> user_ids,
                                                     std::int64_t creator_id);
// `room.update!(name:, type:)`. `name` is set when it holds a value (a nullopt inside clears the name).
[[nodiscard]] Status update(db::Tx& tx, Room& room, std::optional<std::optional<std::string_view>> name,
                            std::optional<std::string_view> type);
// `room.destroy`: the memberships, the messages with what hangs on them, then the room.
[[nodiscard]] Status destroy(db::Tx& tx, const Room& room);
// `memberships.grant_to(users)`: the default involvement of the room class; existing members stay.
[[nodiscard]] Status grant_to(db::Tx& tx, const Room& room, std::span<const std::int64_t> user_ids);
// `memberships.revise(granted:, revoked:)`.
[[nodiscard]] Status revise(db::Tx& tx, const Room& room, std::span<const std::int64_t> granted,
                            std::span<const std::int64_t> revoked);
// `room.user_ids`.
[[nodiscard]] Result<std::vector<std::int64_t>> user_ids(db::Connection& conn, Arena& arena, std::int64_t room_id);
// `room.default_involvement`.
[[nodiscard]] std::string_view default_involvement(std::string_view type);

}  // namespace rooms
}  // namespace campfire::models
