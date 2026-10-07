// Rails: app/models/membership/connectable.rb. Rust: crates/db/src/models/membership.rs (Membership::Connectable).
#include "models/membership_connect.hpp"

#include <optional>
#include <string>
#include <string_view>

#include "core/arena.hpp"
#include "core/time_format.hpp"
#include "db/schema.gen.hpp"

namespace campfire::models::memberships {

namespace {

struct Row {
  std::int64_t id = 0;
  std::optional<std::string_view> connected_at;
  std::int64_t connections = 0;
  static Row read(db::RowReader& r) { return {r.i64(0), r.text_opt(1), r.i64(2)}; }
};

const db::Query<Row(std::int64_t, std::int64_t)> kFind{
    "SELECT \"memberships\".\"id\", \"memberships\".\"connected_at\", \"memberships\".\"connections\" FROM "
    "\"memberships\" WHERE \"memberships\".\"room_id\" = ? AND \"memberships\".\"user_id\" = ? LIMIT 1"};

const db::Query<void(std::int64_t, std::string_view, std::int64_t)> kConnect{
    "UPDATE \"memberships\" SET \"connections\" = ?, \"connected_at\" = ?, \"unread_at\" = NULL WHERE "
    "\"memberships\".\"id\" = ?"};

const db::Query<void(std::int64_t, std::string_view, std::int64_t)> kSetConnections{
    "UPDATE \"memberships\" SET \"connections\" = ?, \"updated_at\" = ? WHERE \"memberships\".\"id\" = ?"};

const db::Query<void(std::string_view, std::int64_t)> kDisconnect{
    "UPDATE \"memberships\" SET \"connected_at\" = NULL, \"updated_at\" = ? WHERE \"memberships\".\"id\" = ?"};

const db::Query<void(std::string_view, std::string_view, std::int64_t)> kTouch{
    "UPDATE \"memberships\" SET \"updated_at\" = ?, \"connected_at\" = ? WHERE \"memberships\".\"id\" = ?"};

// `connected?`: `connected_at? && connected_at >= CONNECTION_TTL.ago`.
bool is_connected(db::Tx& tx, const Row& row) {
  if (!row.connected_at) return false;
  const auto at = parse_db(*row.connected_at);
  if (!at) return false;
  return *at >= tx.now().plus_seconds(-kConnectionTtlSeconds);
}

Result<std::optional<Row>> find(db::Tx& tx, Arena& arena, std::int64_t room_id, std::int64_t user_id) {
  return tx.conn().first(kFind, arena, room_id, user_id);
}

void changed(db::Tx& tx, std::int64_t id) {
  tx.changed(db::schema::Table::Memberships, id);
}

// `update!(connections:)`: nothing is written when the value is the same.
Status set_connections(db::Tx& tx, const Row& row, std::int64_t connections, std::string_view now) {
  if (row.connections == connections) return {};
  auto done = tx.conn().exec(kSetConnections, connections, now, row.id);
  if (!done) return std::unexpected(done.error());
  return {};
}

}  // namespace

Result<bool> present(db::Tx& tx, std::int64_t room_id, std::int64_t user_id) {
  Arena arena(256);
  auto row = find(tx, arena, room_id, user_id);
  if (!row) return std::unexpected(row.error());
  if (!*row) return false;
  const std::int64_t connections = is_connected(tx, **row) ? (*row)->connections + 1 : 1;
  auto done = tx.conn().exec(kConnect, connections, tx.now_db(), (*row)->id);
  if (!done) return std::unexpected(done.error());
  changed(tx, (*row)->id);
  return true;
}

Result<bool> disconnected(db::Tx& tx, std::int64_t room_id, std::int64_t user_id) {
  Arena arena(256);
  auto found = find(tx, arena, room_id, user_id);
  if (!found) return std::unexpected(found.error());
  if (!*found) return false;
  Row row = **found;
  const std::string now = tx.now_db();
  // `decrement_connections`: one less while connected, else none.
  if (is_connected(tx, row)) {
    auto done = set_connections(tx, row, row.connections - 1, now);
    if (!done) return std::unexpected(done.error());
    row.connections -= 1;
  } else {
    auto done = set_connections(tx, row, 0, now);
    if (!done) return std::unexpected(done.error());
    row.connections = 0;
  }
  // `update! connected_at: nil if connections < 1`
  if (row.connections < 1 && row.connected_at) {
    auto done = tx.conn().exec(kDisconnect, now, row.id);
    if (!done) return std::unexpected(done.error());
  }
  changed(tx, row.id);
  return true;
}

Result<bool> refresh_connection(db::Tx& tx, std::int64_t room_id, std::int64_t user_id) {
  Arena arena(256);
  auto found = find(tx, arena, room_id, user_id);
  if (!found) return std::unexpected(found.error());
  if (!*found) return false;
  const Row row = **found;
  const std::string now = tx.now_db();
  // `increment_connections unless connected?`: a connection that expired starts again at one.
  if (!is_connected(tx, row)) {
    auto done = set_connections(tx, row, 1, now);
    if (!done) return std::unexpected(done.error());
  }
  // `touch :connected_at`
  auto touched = tx.conn().exec(kTouch, now, now, row.id);
  if (!touched) return std::unexpected(touched.error());
  changed(tx, row.id);
  return true;
}

}  // namespace campfire::models::memberships
