// Rails: app/models/room.rb, app/models/rooms/*.rb. Rust: crates/db/src/models/room.rs.
#include "models/room.hpp"

#include <set>

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

const db::Query<std::int64_t(std::string_view, std::int64_t, std::optional<std::string_view>, std::string_view,
                             std::string_view)>
    kInsert{
        "INSERT INTO \"rooms\" (\"created_at\", \"creator_id\", \"name\", \"type\", \"updated_at\") VALUES "
        "(?, ?, ?, ?, ?) RETURNING \"id\""};

const db::Query<void(std::string_view, std::string_view, std::int64_t, std::string_view, std::int64_t)> kGrant{
    "INSERT INTO \"memberships\" (\"created_at\", \"involvement\", \"room_id\", \"updated_at\", \"user_id\") "
    "VALUES (?, ?, ?, ?, ?) ON CONFLICT DO NOTHING"};

const db::Query<void(std::int64_t, std::int64_t)> kRevoke{
    "DELETE FROM \"memberships\" WHERE \"memberships\".\"room_id\" = ? AND \"memberships\".\"user_id\" = ?"};

const db::Query<void(std::optional<std::string_view>, std::string_view, std::string_view, std::int64_t)> kUpdate{
    "UPDATE \"rooms\" SET \"name\" = ?, \"type\" = ?, \"updated_at\" = ? WHERE \"rooms\".\"id\" = ?"};

const db::Query<std::int64_t()> kActiveUserIds{"SELECT \"users\".\"id\" FROM \"users\" WHERE \"users\".\"status\" = 0"};

const db::Query<std::int64_t(std::int64_t)> kUserIds{
    "SELECT \"users\".\"id\" FROM \"users\" INNER JOIN \"memberships\" ON \"users\".\"id\" = "
    "\"memberships\".\"user_id\" WHERE \"memberships\".\"room_id\" = ?"};

const db::Query<std::int64_t()> kDirectRoomIds{
    "SELECT \"rooms\".\"id\" FROM \"rooms\" WHERE \"rooms\".\"type\" = 'Rooms::Direct' ORDER BY \"rooms\".\"id\""};

const db::Query<db::schema::RoomRow(std::int64_t)> kById{
    "SELECT \"rooms\".\"id\", \"rooms\".\"created_at\", \"rooms\".\"creator_id\", \"rooms\".\"name\", "
    "\"rooms\".\"type\", \"rooms\".\"updated_at\" FROM \"rooms\" WHERE \"rooms\".\"id\" = ? LIMIT 1"};

// What hangs on the messages of a room (Rust: Message#destroy). Active Storage blobs are purged later (A3).
const db::Query<void(std::int64_t)> kDeleteBoosts{
    "DELETE FROM \"boosts\" WHERE \"boosts\".\"message_id\" IN (SELECT \"messages\".\"id\" FROM \"messages\" WHERE "
    "\"messages\".\"room_id\" = ?)"};
const db::Query<void(std::int64_t)> kDeleteRichTexts{
    "DELETE FROM \"action_text_rich_texts\" WHERE \"action_text_rich_texts\".\"record_type\" = 'Message' AND "
    "\"action_text_rich_texts\".\"record_id\" IN (SELECT \"messages\".\"id\" FROM \"messages\" WHERE "
    "\"messages\".\"room_id\" = ?)"};
const db::Query<void(std::int64_t)> kDeleteAttachments{
    "DELETE FROM \"active_storage_attachments\" WHERE \"active_storage_attachments\".\"record_type\" = 'Message' AND "
    "\"active_storage_attachments\".\"record_id\" IN (SELECT \"messages\".\"id\" FROM \"messages\" WHERE "
    "\"messages\".\"room_id\" = ?)"};
const db::Query<void(std::int64_t)> kDeleteSearchIndex{
    "DELETE FROM message_search_index WHERE rowid IN (SELECT \"messages\".\"id\" FROM \"messages\" WHERE "
    "\"messages\".\"room_id\" = ?)"};
const db::Query<void(std::int64_t)> kDeleteMessages{"DELETE FROM \"messages\" WHERE \"messages\".\"room_id\" = ?"};
const db::Query<void(std::int64_t)> kDeleteMemberships{
    "DELETE FROM \"memberships\" WHERE \"memberships\".\"room_id\" = ?"};
const db::Query<void(std::int64_t)> kDeleteRoom{"DELETE FROM \"rooms\" WHERE \"rooms\".\"id\" = ?"};

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

Result<std::optional<Room>> find_by_id(db::Connection& conn, Arena& arena, std::int64_t room_id) {
  return wrap(conn.first(kById, arena, room_id));
}

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

std::string_view default_involvement(std::string_view type) {
  return type == kRoomDirect ? "everything" : "mentions";
}

Result<std::vector<std::int64_t>> user_ids(db::Connection& conn, Arena& arena, std::int64_t room_id) {
  auto rows = conn.all(kUserIds, arena, room_id);
  if (!rows) return std::unexpected(rows.error());
  return std::vector<std::int64_t>(rows->begin(), rows->end());
}

Status grant_to(db::Tx& tx, const Room& room, std::span<const std::int64_t> ids) {
  const std::string now = tx.now_db();
  const std::string_view involvement = default_involvement(room.type);
  for (const std::int64_t user_id : ids) {
    auto done = tx.conn().exec(kGrant, now, involvement, room.id, now, user_id);
    if (!done) return std::unexpected(done.error());
  }
  tx.changed(db::schema::Table::Memberships, room.id);
  return {};
}

namespace {

Status revoke_from(db::Tx& tx, const Room& room, std::span<const std::int64_t> ids) {
  for (const std::int64_t user_id : ids) {
    auto done = tx.conn().exec(kRevoke, room.id, user_id);
    if (!done) return std::unexpected(done.error());
  }
  tx.changed(db::schema::Table::Memberships, room.id);
  return {};
}

// `Room.original` is not needed here; an open room grants itself to every active user.
Status grant_to_active_users(db::Tx& tx, const Room& room) {
  Arena arena(1024);
  auto ids = tx.conn().all(kActiveUserIds, arena);
  if (!ids) return std::unexpected(ids.error());
  const std::vector<std::int64_t> list(ids->begin(), ids->end());
  return grant_to(tx, room, list);
}

}  // namespace

Status revise(db::Tx& tx, const Room& room, std::span<const std::int64_t> granted,
              std::span<const std::int64_t> revoked) {
  if (!granted.empty()) {
    if (auto done = grant_to(tx, room, granted); !done) return done;
  }
  if (!revoked.empty()) {
    if (auto done = revoke_from(tx, room, revoked); !done) return done;
  }
  return {};
}

Result<Room> create(db::Tx& tx, std::string_view type, std::optional<std::string_view> name, std::int64_t creator_id) {
  const std::string now = tx.now_db();
  Arena arena(1024);
  auto id = tx.conn().first(kInsert, arena, now, creator_id, name, type, now);
  if (!id) return std::unexpected(id.error());
  Room room;
  room.id = **id;
  room.created_at = room.updated_at = now;
  room.creator_id = creator_id;
  if (name) room.name = std::string(*name);
  room.type = std::string(type);
  tx.changed(db::schema::Table::Rooms, room.id);
  if (room.is_open()) {
    if (auto done = grant_to_active_users(tx, room); !done) return std::unexpected(done.error());
  }
  return room;
}

Result<Room> create_for(db::Tx& tx, std::string_view type, std::optional<std::string_view> name,
                        std::int64_t creator_id, std::span<const std::int64_t> ids) {
  auto room = create(tx, type, name, creator_id);
  if (!room) return room;
  if (auto done = grant_to(tx, *room, ids); !done) return std::unexpected(done.error());
  return room;
}

Result<Room> find_or_create_direct_for(db::Tx& tx, std::span<const std::int64_t> ids, std::int64_t creator_id) {
  Arena arena(4096);
  const std::set<std::int64_t> wanted(ids.begin(), ids.end());
  auto candidates = tx.conn().all(kDirectRoomIds, arena);
  if (!candidates) return std::unexpected(candidates.error());
  for (const std::int64_t room_id : *candidates) {
    auto members = user_ids(tx.conn(), arena, room_id);
    if (!members) return std::unexpected(members.error());
    if (std::set<std::int64_t>(members->begin(), members->end()) != wanted) continue;
    auto row = tx.conn().first(kById, arena, room_id);
    if (!row) return std::unexpected(row.error());
    if (*row) return Room::from_row(**row);
  }
  return create_for(tx, kRoomDirect, std::nullopt, creator_id, ids);
}

Status update(db::Tx& tx, Room& room, std::optional<std::optional<std::string_view>> name,
              std::optional<std::string_view> type) {
  const auto same_name = [&] {
    if (!name) return true;
    const std::optional<std::string_view> current =
        room.name ? std::optional<std::string_view>(*room.name) : std::nullopt;
    return *name == current;
  };
  const bool type_changes = type && *type != room.type;
  // `direct_rooms_keep_their_type`: a direct room keeps its class.
  if (type_changes && room.is_direct()) {
    return std::unexpected(Error{Errc::InvalidArgument, "Validation failed: Type can't be changed for a direct room"});
  }
  if (same_name() && !type_changes) return {};
  if (name) room.name = *name ? std::optional<std::string>(std::string(**name)) : std::nullopt;
  if (type_changes) room.type = std::string(*type);
  room.updated_at = tx.now_db();
  const std::optional<std::string_view> stored = room.name ? std::optional<std::string_view>(*room.name) : std::nullopt;
  auto done = tx.conn().exec(kUpdate, stored, room.type, room.updated_at, room.id);
  if (!done) return std::unexpected(done.error());
  tx.changed(db::schema::Table::Rooms, room.id);
  if (type_changes && room.is_open()) return grant_to_active_users(tx, room);
  return {};
}

Status destroy(db::Tx& tx, const Room& room) {
  for (const auto* query : {&kDeleteMemberships}) {
    auto done = tx.conn().exec(*query, room.id);
    if (!done) return std::unexpected(done.error());
  }
  for (const auto* query :
       {&kDeleteBoosts, &kDeleteRichTexts, &kDeleteAttachments, &kDeleteSearchIndex, &kDeleteMessages}) {
    auto done = tx.conn().exec(*query, room.id);
    if (!done) return std::unexpected(done.error());
  }
  auto done = tx.conn().exec(kDeleteRoom, room.id);
  if (!done) return std::unexpected(done.error());
  tx.changed(db::schema::Table::Rooms, room.id);
  tx.changed(db::schema::Table::Memberships, room.id);
  tx.changed(db::schema::Table::Messages, room.id);
  return {};
}

}  // namespace rooms
}  // namespace campfire::models
