// Rails: app/models/membership.rb. Rust: crates/db/src/models/membership.rs.
#include "models/membership.hpp"

namespace campfire::models {

namespace {

// A membership and its room, in the order of the SELECT list below.
struct JoinedRow {
  db::schema::MembershipRow membership;
  db::schema::RoomRow room;

  static JoinedRow read(db::RowReader& r) {
    JoinedRow row;
    row.membership.id = r.i64(0);
    row.membership.involvement = r.text_opt(1);
    row.membership.room_id = r.i64(2);
    row.membership.unread_at = r.text_opt(3);
    row.membership.updated_at = r.text(4);
    row.membership.user_id = r.i64(5);
    row.membership.created_at = r.text(6);
    row.room.id = r.i64(7);
    row.room.created_at = r.text(8);
    row.room.creator_id = r.i64(9);
    row.room.name = r.text_opt(10);
    row.room.type = r.text(11);
    row.room.updated_at = r.text(12);
    return row;
  }
};

const db::Query<JoinedRow(std::int64_t)> kVisibleWithOrderedRoom{
    "SELECT \"memberships\".\"id\", \"memberships\".\"involvement\", \"memberships\".\"room_id\", "
    "\"memberships\".\"unread_at\", \"memberships\".\"updated_at\", \"memberships\".\"user_id\", "
    "\"memberships\".\"created_at\", \"rooms\".\"id\", \"rooms\".\"created_at\", \"rooms\".\"creator_id\", "
    "\"rooms\".\"name\", \"rooms\".\"type\", \"rooms\".\"updated_at\" FROM \"memberships\" INNER JOIN \"rooms\" ON "
    "\"rooms\".\"id\" = \"memberships\".\"room_id\" WHERE \"memberships\".\"user_id\" = ? AND "
    "\"memberships\".\"involvement\" != 'invisible' ORDER BY LOWER(rooms.name)"};

}  // namespace

namespace memberships {

Result<std::vector<MembershipWithRoom>> visible_with_ordered_room(db::Connection& conn, Arena& arena,
                                                                  std::int64_t user_id) {
  auto rows = conn.all(kVisibleWithOrderedRoom, arena, user_id);
  if (!rows) return std::unexpected(rows.error());
  std::vector<MembershipWithRoom> out;
  out.reserve(rows->size());
  for (const JoinedRow& row : *rows) {
    Membership m;
    m.id = row.membership.id;
    m.room_id = row.membership.room_id;
    m.user_id = row.membership.user_id;
    if (row.membership.involvement) m.involvement = std::string(*row.membership.involvement);
    if (row.membership.unread_at) m.unread_at = std::string(*row.membership.unread_at);
    m.created_at = std::string(row.membership.created_at);
    m.updated_at = std::string(row.membership.updated_at);
    out.push_back({std::move(m), Room::from_row(row.room)});
  }
  return out;
}

}  // namespace memberships
}  // namespace campfire::models
