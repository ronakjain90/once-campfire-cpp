// Rails: app/models/membership.rb. Rust: crates/db/src/models/membership.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/arena.hpp"
#include "core/error.hpp"
#include "db/connection.hpp"
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

// `user.memberships.visible.with_ordered_room`: not invisible, ordered by `LOWER(rooms.name)`.
[[nodiscard]] Result<std::vector<MembershipWithRoom>> visible_with_ordered_room(db::Connection& conn, Arena& arena,
                                                                                std::int64_t user_id);

}  // namespace memberships
}  // namespace campfire::models
