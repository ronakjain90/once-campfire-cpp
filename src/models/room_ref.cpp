// Rails: app/models/room.rb, app/models/user.rb. Rust: crates/db/src/models/room.rs, user.rs.
#include "models/room_ref.hpp"

#include <algorithm>

namespace campfire::models {

namespace {

using db::schema::RoomRow;
using db::schema::UserRow;

#define CF_ROOM_COLUMNS                                                                                     \
  "\"rooms\".\"id\", \"rooms\".\"created_at\", \"rooms\".\"creator_id\", \"rooms\".\"name\", \"rooms\".\"type\", " \
  "\"rooms\".\"updated_at\""

const db::Query<RoomRow(std::int64_t, std::int64_t)> kForUser{
    "SELECT " CF_ROOM_COLUMNS
    " FROM \"rooms\" INNER JOIN \"memberships\" ON \"rooms\".\"id\" = \"memberships\".\"room_id\" WHERE "
    "\"memberships\".\"user_id\" = ? AND \"rooms\".\"id\" = ? LIMIT 1"};
const db::Query<RoomRow(std::int64_t)> kById{"SELECT " CF_ROOM_COLUMNS
                                             " FROM \"rooms\" WHERE \"rooms\".\"id\" = ? LIMIT 1"};
const db::Query<std::int64_t(std::int64_t)> kMemberIds{
    "SELECT \"memberships\".\"user_id\" FROM \"memberships\" WHERE \"memberships\".\"room_id\" = ?"};

#define CF_USERS "SELECT " CF_USER_COLS " FROM \"users\""
#define CF_USER_COLS                                                                                     \
  "\"users\".\"id\", \"users\".\"bio\", \"users\".\"bot_token\", \"users\".\"created_at\", "           \
  "\"users\".\"email_address\", \"users\".\"name\", \"users\".\"password_digest\", \"users\".\"role\", " \
  "\"users\".\"status\", \"users\".\"updated_at\""
#define CF_MEMBER_JOIN " INNER JOIN \"memberships\" ON \"users\".\"id\" = \"memberships\".\"user_id\""

const db::Query<UserRow(std::int64_t)> kActiveBots{
    CF_USERS CF_MEMBER_JOIN
    " WHERE \"memberships\".\"room_id\" = ? AND \"users\".\"status\" = 0 AND \"users\".\"role\" = 2"};
// `where(id: ids)` with the ids as a JSON array: one statement for any number of ids.
const db::Query<UserRow(std::int64_t, std::string_view)> kMembersAmong{
    CF_USERS CF_MEMBER_JOIN
    " WHERE \"memberships\".\"room_id\" = ? AND \"users\".\"id\" IN (SELECT value FROM json_each(?))"};
// The autocomplete scopes: `users_scope.active[.filtered_by(query)].ordered`.
const db::Query<UserRow()> kAllActive{CF_USERS " WHERE \"users\".\"status\" = 0 ORDER BY LOWER(name)"};
const db::Query<UserRow(std::string_view)> kAllActiveFiltered{
    CF_USERS " WHERE \"users\".\"status\" = 0 AND (name like ?) ORDER BY LOWER(name)"};
const db::Query<UserRow(std::int64_t)> kRoomActive{
    CF_USERS CF_MEMBER_JOIN " WHERE \"memberships\".\"room_id\" = ? AND \"users\".\"status\" = 0 ORDER BY LOWER(name)"};
const db::Query<UserRow(std::int64_t, std::string_view)> kRoomActiveFiltered{
    CF_USERS CF_MEMBER_JOIN
    " WHERE \"memberships\".\"room_id\" = ? AND \"users\".\"status\" = 0 AND (name like ?) ORDER BY LOWER(name)"};

Result<std::vector<User>> users_of(Result<std::pmr::vector<UserRow>> rows) {
  if (!rows) return std::unexpected(rows.error());
  std::vector<User> out;
  out.reserve(rows->size());
  for (const UserRow& row : *rows) out.push_back(User::from_row(row));
  return out;
}

}  // namespace

std::string RoomRef::param_key() const {
  std::string key = type;
  for (std::size_t at = key.find("::"); at != std::string::npos; at = key.find("::")) key.replace(at, 2, "_");
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
  return key;
}

namespace room_refs {

namespace {
Result<std::optional<RoomRef>> wrap(Result<std::optional<RoomRow>> row) {
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<RoomRef>{};
  const RoomRow& r = **row;
  RoomRef room;
  room.id = r.id;
  room.created_at = std::string(r.created_at);
  room.creator_id = r.creator_id;
  if (r.name) room.name = std::string(*r.name);
  room.type = std::string(r.type);
  room.updated_at = std::string(r.updated_at);
  return std::optional<RoomRef>(std::move(room));
}
}  // namespace

Result<std::optional<RoomRef>> find_for_user(db::Connection& conn, Arena& arena, std::int64_t user_id,
                                             std::int64_t room_id) {
  return wrap(conn.first(kForUser, arena, user_id, room_id));
}

Result<std::optional<RoomRef>> find(db::Connection& conn, Arena& arena, std::int64_t room_id) {
  return wrap(conn.first(kById, arena, room_id));
}

Result<std::vector<std::int64_t>> member_user_ids(db::Connection& conn, Arena& arena, std::int64_t room_id) {
  auto rows = conn.all(kMemberIds, arena, room_id);
  if (!rows) return std::unexpected(rows.error());
  return std::vector<std::int64_t>(rows->begin(), rows->end());
}

Result<std::vector<User>> active_bots(db::Connection& conn, Arena& arena, std::int64_t room_id) {
  return users_of(conn.all(kActiveBots, arena, room_id));
}

Result<std::vector<User>> members_among(db::Connection& conn, Arena& arena, std::int64_t room_id,
                                        const std::vector<std::int64_t>& user_ids) {
  if (user_ids.empty()) return std::vector<User>{};
  std::string json = "[";
  for (std::size_t i = 0; i < user_ids.size(); ++i) json += (i ? "," : "") + std::to_string(user_ids[i]);
  json += "]";
  return users_of(conn.all(kMembersAmong, arena, room_id, std::string_view(json)));
}

Result<std::vector<User>> autocompletable_users(db::Connection& conn, Arena& arena,
                                                std::optional<std::int64_t> room_id,
                                                std::optional<std::string_view> query) {
  const std::string pattern = query ? "%" + std::string(*query) + "%" : std::string();
  if (room_id) {
    return users_of(query ? conn.all(kRoomActiveFiltered, arena, *room_id, std::string_view(pattern))
                          : conn.all(kRoomActive, arena, *room_id));
  }
  return users_of(query ? conn.all(kAllActiveFiltered, arena, std::string_view(pattern))
                        : conn.all(kAllActive, arena));
}

}  // namespace room_refs
}  // namespace campfire::models
