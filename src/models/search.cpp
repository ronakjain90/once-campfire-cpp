// Rails: app/models/search.rb, app/models/message/searchable.rb. Rust: crates/db/src/models/search.rs, message.rs.
#include "models/search.hpp"

#include <algorithm>

namespace campfire::models {

namespace {

const db::Query<std::string_view(std::int64_t)> kRecent{
    "SELECT \"searches\".\"query\" FROM \"searches\" WHERE \"searches\".\"user_id\" = ? ORDER BY "
    "\"searches\".\"updated_at\" DESC"};
const db::Query<std::int64_t(std::int64_t, std::string_view)> kFind{
    "SELECT \"searches\".\"id\" FROM \"searches\" WHERE \"searches\".\"user_id\" = ? AND \"searches\".\"query\" = ? "
    "LIMIT 1"};
const db::Query<std::int64_t(std::string_view, std::string_view, std::string_view, std::int64_t)> kInsert{
    "INSERT INTO \"searches\" (\"created_at\", \"query\", \"updated_at\", \"user_id\") VALUES (?, ?, ?, ?) RETURNING "
    "\"id\""};
// `user.searches.excluding(user.searches.ordered.limit(10)).destroy_all`, as one statement.
const db::Query<void(std::int64_t, std::int64_t, std::int64_t)> kTrim{
    "DELETE FROM \"searches\" WHERE \"searches\".\"user_id\" = ? AND \"searches\".\"id\" NOT IN (SELECT "
    "\"searches\".\"id\" FROM \"searches\" WHERE \"searches\".\"user_id\" = ? ORDER BY \"searches\".\"updated_at\" "
    "DESC LIMIT ?)"};
const db::Query<void(std::string_view, std::int64_t)> kTouch{
    "UPDATE \"searches\" SET \"updated_at\" = ? WHERE \"searches\".\"id\" = ?"};
const db::Query<void(std::int64_t)> kDestroyAll{"DELETE FROM \"searches\" WHERE \"searches\".\"user_id\" = ?"};

using db::schema::MessageRow;

const db::Query<MessageRow(std::int64_t, std::string_view)> kSearchReachable{
    "SELECT \"messages\".\"id\", \"messages\".\"client_message_id\", \"messages\".\"created_at\", "
    "\"messages\".\"creator_id\", \"messages\".\"room_id\", \"messages\".\"updated_at\" FROM \"messages\" INNER JOIN "
    "\"rooms\" ON \"messages\".\"room_id\" = \"rooms\".\"id\" INNER JOIN \"memberships\" ON \"rooms\".\"id\" = "
    "\"memberships\".\"room_id\" join message_search_index idx on messages.id = idx.rowid WHERE "
    "\"memberships\".\"user_id\" = ? AND (idx.body match ?) ORDER BY \"messages\".\"created_at\" DESC LIMIT 100"};

}  // namespace

namespace searches {

Result<std::vector<std::string>> recent_queries(db::Connection& conn, Arena& arena, std::int64_t user_id) {
  auto rows = conn.all(kRecent, arena, user_id);
  if (!rows) return std::unexpected(rows.error());
  std::vector<std::string> out;
  out.reserve(rows->size());
  for (const std::string_view query : *rows) out.emplace_back(query);
  return out;
}

Status record(db::Tx& tx, std::int64_t user_id, std::string_view query) {
  Arena arena(256);
  auto found = tx.conn().first(kFind, arena, user_id, query);
  if (!found) return std::unexpected(found.error());
  const std::string now = tx.now_db();
  std::int64_t id = 0;
  if (*found) {
    id = **found;
  } else {
    auto created = tx.conn().first(kInsert, arena, now, query, now, user_id);
    if (!created) return std::unexpected(created.error());
    id = created->value_or(0);
    auto trimmed = tx.conn().exec(kTrim, user_id, user_id, kRecentSearches);
    if (!trimmed) return std::unexpected(trimmed.error());
  }
  auto touched = tx.conn().exec(kTouch, now, id);
  if (!touched) return std::unexpected(touched.error());
  tx.changed(db::schema::Table::Searches, id);
  return {};
}

Status destroy_all(db::Tx& tx, std::int64_t user_id) {
  auto done = tx.conn().exec(kDestroyAll, user_id);
  if (!done) return std::unexpected(done.error());
  tx.changed(db::schema::Table::Searches, user_id);
  return {};
}

}  // namespace searches

namespace messages {

Result<std::vector<Message>> search_reachable(db::Connection& conn, Arena& arena, std::int64_t user_id,
                                              std::string_view terms) {
  std::vector<Message> out;
  if (terms.empty()) return out;
  auto rows = conn.all(kSearchReachable, arena, user_id, terms);
  if (!rows) return std::unexpected(rows.error());
  out.reserve(rows->size());
  for (auto it = rows->rbegin(); it != rows->rend(); ++it) out.push_back(Message::from_row(*it));
  return out;
}

}  // namespace messages
}  // namespace campfire::models
