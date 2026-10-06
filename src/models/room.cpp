// Rails: app/models/room.rb, app/models/rooms/*.rb. Rust: crates/db/src/models/room.rs.
#include "models/room.hpp"

namespace campfire::models {

namespace {

#define CF_ROOM_JOIN                                                                                    \
  " FROM \"rooms\" INNER JOIN \"memberships\" ON \"rooms\".\"id\" = \"memberships\".\"room_id\" WHERE " \
  "\"memberships\".\"user_id\" = ?"

const db::Query<db::schema::RoomRow(std::int64_t)> kLast{
    "SELECT \"rooms\".\"id\", \"rooms\".\"created_at\", \"rooms\".\"creator_id\", \"rooms\".\"name\", "
    "\"rooms\".\"type\", \"rooms\".\"updated_at\"" CF_ROOM_JOIN " ORDER BY \"rooms\".\"id\" DESC LIMIT 1"};

const db::Query<db::schema::RoomRow(std::int64_t)> kOriginal{
    "SELECT \"rooms\".\"id\", \"rooms\".\"created_at\", \"rooms\".\"creator_id\", \"rooms\".\"name\", "
    "\"rooms\".\"type\", \"rooms\".\"updated_at\"" CF_ROOM_JOIN " ORDER BY \"rooms\".\"created_at\" ASC LIMIT 1"};

const db::Query<db::schema::RoomRow(std::int64_t, std::int64_t)> kFindAll{
    "SELECT \"rooms\".\"id\", \"rooms\".\"created_at\", \"rooms\".\"creator_id\", \"rooms\".\"name\", "
    "\"rooms\".\"type\", \"rooms\".\"updated_at\"" CF_ROOM_JOIN " AND \"rooms\".\"id\" = ? LIMIT 1"};

const db::Query<db::schema::RoomRow(std::int64_t, std::int64_t)> kFindWithoutDirects{
    "SELECT \"rooms\".\"id\", \"rooms\".\"created_at\", \"rooms\".\"creator_id\", \"rooms\".\"name\", "
    "\"rooms\".\"type\", \"rooms\".\"updated_at\"" CF_ROOM_JOIN
    " AND \"rooms\".\"type\" != 'Rooms::Direct' AND \"rooms\".\"id\" = ? LIMIT 1"};

const db::Query<db::schema::RoomRow(std::int64_t, std::int64_t)> kFindDirects{
    "SELECT \"rooms\".\"id\", \"rooms\".\"created_at\", \"rooms\".\"creator_id\", \"rooms\".\"name\", "
    "\"rooms\".\"type\", \"rooms\".\"updated_at\"" CF_ROOM_JOIN
    " AND \"rooms\".\"type\" = 'Rooms::Direct' AND \"rooms\".\"id\" = ? LIMIT 1"};

Result<std::optional<Room>> wrap(Result<std::optional<db::schema::RoomRow>> row) {
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<Room>{};
  return std::optional<Room>(Room::from_row(**row));
}

}  // namespace

Room Room::from_row(const db::schema::RoomRow& row) {
  Room r;
  r.id = row.id;
  r.created_at = std::string(row.created_at);
  r.creator_id = row.creator_id;
  if (row.name) r.name = std::string(*row.name);
  r.type = std::string(row.type);
  r.updated_at = std::string(row.updated_at);
  return r;
}

namespace rooms {

Result<std::optional<Room>> last_of_user(db::Connection& conn, Arena& arena, std::int64_t user_id) {
  return wrap(conn.first(kLast, arena, user_id));
}

Result<std::optional<Room>> original_of_user(db::Connection& conn, Arena& arena, std::int64_t user_id) {
  return wrap(conn.first(kOriginal, arena, user_id));
}

Result<std::optional<Room>> find_for_user(db::Connection& conn, Arena& arena, std::int64_t user_id, RoomScope scope,
                                          std::int64_t room_id) {
  switch (scope) {
    case RoomScope::All: return wrap(conn.first(kFindAll, arena, user_id, room_id));
    case RoomScope::WithoutDirects: return wrap(conn.first(kFindWithoutDirects, arena, user_id, room_id));
    case RoomScope::Directs: return wrap(conn.first(kFindDirects, arena, user_id, room_id));
  }
  return std::optional<Room>{};
}

}  // namespace rooms
}  // namespace campfire::models
