// Rails: app/models/membership/connectable.rb. Rust: crates/db/src/models/membership.rs (Membership::Connectable).
#pragma once

#include <cstdint>

#include "core/error.hpp"
#include "db/database.hpp"

namespace campfire::models::memberships {

// How long a connection counts as alive without a refresh (`CONNECTION_TTL`).
inline constexpr std::int64_t kConnectionTtlSeconds = 60;

// Each function finds `room.memberships.find_by(user:)` and changes it. The result is false if the user has no
// membership in the room (Rails raises NoMethodError on the nil).
//
// `membership.present`: connected now, with one connection more (or one), and read.
[[nodiscard]] Result<bool> present(db::Tx& tx, std::int64_t room_id, std::int64_t user_id);
// `membership.disconnected`: one connection less, and not connected when none is left.
[[nodiscard]] Result<bool> disconnected(db::Tx& tx, std::int64_t room_id, std::int64_t user_id);
// `membership.refresh_connection`: connected now.
[[nodiscard]] Result<bool> refresh_connection(db::Tx& tx, std::int64_t room_id, std::int64_t user_id);

}  // namespace campfire::models::memberships
