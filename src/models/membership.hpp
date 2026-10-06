// Rails: app/models/membership.rb. Rust: crates/db/src/models/membership.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/arena.hpp"
#include "core/error.hpp"
#include "db/connection.hpp"
#include "db/database.hpp"
#include "models/room.hpp"

namespace campfire::models {

// A membership row that owns its text.
struct Membership {
  std::int64_t id = 0;
  std::int64_t room_id = 0;
  std::int64_t user_id = 0;
  std::optional<std::string> involvement;
  std::optional<std::string> unread_at;
  std::string created_at;
  std::string updated_at;

  // `unread?`
  [[nodiscard]] bool unread() const noexcept { return unread_at.has_value(); }
};

struct MembershipWithRoom {
  Membership membership;
  Room room;
};

namespace memberships {

// `Involvement`: the values that `involvement` can take.
[[nodiscard]] bool is_valid_involvement(std::string_view name);

// `user.memberships.find_by(room_id:)`.
[[nodiscard]] Result<std::optional<Membership>> find_for_user_and_room(db::Connection& conn, Arena& arena,
                                                                       std::int64_t user_id, std::int64_t room_id);
// `room.memberships`: in the order of the rows.
[[nodiscard]] Result<std::vector<Membership>> for_room(db::Connection& conn, Arena& arena, std::int64_t room_id);
// `membership.update!(involvement:)`: nothing is written if the value is the same.
[[nodiscard]] Status update_involvement(db::Tx& tx, Membership& membership, std::optional<std::string_view> value);

// `user.memberships.visible.with_ordered_room`: not invisible, ordered by `LOWER(rooms.name)`.
[[nodiscard]] Result<std::vector<MembershipWithRoom>> visible_with_ordered_room(db::Connection& conn, Arena& arena,
                                                                                std::int64_t user_id);

}  // namespace memberships
}  // namespace campfire::models
