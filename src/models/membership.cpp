// Rails: app/models/membership.rb. Rust: crates/db/src/models/membership.rs.
#include "models/membership.hpp"

namespace campfire::models {

namespace {

#define CF_MEMBERSHIP_COLUMNS                                                                      \
  "\"memberships\".\"id\", \"memberships\".\"connected_at\", \"memberships\".\"connections\", "    \
  "\"memberships\".\"created_at\", \"memberships\".\"involvement\", \"memberships\".\"room_id\", " \
  "\"memberships\".\"unread_at\", \"memberships\".\"updated_at\", \"memberships\".\"user_id\""

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

const db::Query<db::schema::MembershipRow(std::int64_t, std::int64_t)> kForUserAndRoom{
    "SELECT " CF_MEMBERSHIP_COLUMNS
    " FROM \"memberships\" WHERE \"memberships\".\"user_id\" = ? AND "
    "\"memberships\".\"room_id\" = ? LIMIT 1"};

const db::Query<db::schema::MembershipRow(std::int64_t)> kForRoom{
    "SELECT " CF_MEMBERSHIP_COLUMNS " FROM \"memberships\" WHERE \"memberships\".\"room_id\" = ?"};

const db::Query<void(std::optional<std::string_view>, std::string_view, std::int64_t)> kUpdateInvolvement{
    "UPDATE \"memberships\" SET \"involvement\" = ?, \"updated_at\" = ? WHERE \"memberships\".\"id\" = ?"};

Membership own(const db::schema::MembershipRow& row) {
  Membership m;
  m.id = row.id;
  m.room_id = row.room_id;
  m.user_id = row.user_id;
  if (row.involvement) m.involvement = std::string(*row.involvement);
  if (row.unread_at) m.unread_at = std::string(*row.unread_at);
  m.created_at = std::string(row.created_at);
  m.updated_at = std::string(row.updated_at);
  return m;
}

}  // namespace

namespace memberships {

bool is_valid_involvement(std::string_view name) {
  return name == "invisible" || name == "nothing" || name == "mentions" || name == "everything";
}

Result<std::optional<Membership>> find_for_user_and_room(db::Connection& conn, Arena& arena, std::int64_t user_id,
                                                         std::int64_t room_id) {
  auto row = conn.first(kForUserAndRoom, arena, user_id, room_id);
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<Membership>{};
  return std::optional<Membership>(own(**row));
}

Result<std::vector<Membership>> for_room(db::Connection& conn, Arena& arena, std::int64_t room_id) {
  auto rows = conn.all(kForRoom, arena, room_id);
  if (!rows) return std::unexpected(rows.error());
  std::vector<Membership> out;
  out.reserve(rows->size());
  for (const db::schema::MembershipRow& row : *rows) out.push_back(own(row));
  return out;
}

Status update_involvement(db::Tx& tx, Membership& membership, std::optional<std::string_view> value) {
  const std::optional<std::string> next = value ? std::optional<std::string>(std::string(*value)) : std::nullopt;
  if (membership.involvement == next) return {};
  const std::string now = tx.now_db();
  auto done = tx.conn().exec(kUpdateInvolvement, value, now, membership.id);
  if (!done) return std::unexpected(done.error());
  membership.involvement = next;
  membership.updated_at = now;
  tx.changed(db::schema::Table::Memberships, membership.id);
  return {};
}

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
