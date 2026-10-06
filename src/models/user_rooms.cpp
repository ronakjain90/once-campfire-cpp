// `Current.user.rooms`. Rust: crates/db/src/models/room.rs (SELECT_FOR_USER).
#include "models/user_rooms.hpp"

namespace campfire::models::user_rooms {

namespace {

struct IdRow {
  std::int64_t id;
  static IdRow read(db::RowReader& r) { return {r.i64(0)}; }
};

#define CF_ROOMS_FOR_USER \
  " FROM \"rooms\" INNER JOIN \"memberships\" ON \"rooms\".\"id\" = \"memberships\".\"room_id\" WHERE \"memberships\".\"user_id\" = ?"

const db::Query<std::int64_t(std::int64_t)> kAny{"SELECT 1 AS one" CF_ROOMS_FOR_USER " LIMIT 1"};
const db::Query<IdRow(std::int64_t, std::int64_t)> kFind{"SELECT \"rooms\".\"id\"" CF_ROOMS_FOR_USER
                                                         " AND \"rooms\".\"id\" = ? LIMIT 1"};
const db::Query<IdRow(std::int64_t)> kOriginal{"SELECT \"rooms\".\"id\"" CF_ROOMS_FOR_USER
                                               " ORDER BY \"rooms\".\"created_at\" ASC LIMIT 1"};

}  // namespace

Result<bool> any(db::Connection& conn, Arena& arena, std::int64_t user_id) {
  auto row = conn.first(kAny, arena, user_id);
  if (!row) return std::unexpected(row.error());
  return row->has_value();
}

Result<std::optional<std::int64_t>> find(db::Connection& conn, Arena& arena, std::int64_t user_id, std::int64_t room_id) {
  auto row = conn.first(kFind, arena, user_id, room_id);
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<std::int64_t>{};
  return std::optional<std::int64_t>((*row)->id);
}

Result<std::optional<std::int64_t>> original(db::Connection& conn, Arena& arena, std::int64_t user_id) {
  auto row = conn.first(kOriginal, arena, user_id);
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<std::int64_t>{};
  return std::optional<std::int64_t>((*row)->id);
}

}  // namespace campfire::models::user_rooms
