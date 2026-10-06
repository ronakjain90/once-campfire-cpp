// Rails: app/models/boost.rb. Rust: crates/db/src/models/boost.rs.
#include "models/boost.hpp"

#include "models/message.hpp"

namespace campfire::models {

namespace {

using db::schema::BoostRow;

#define CF_BOOST_COLUMNS                                                                                  \
  "\"boosts\".\"id\", \"boosts\".\"message_id\", \"boosts\".\"booster_id\", \"boosts\".\"content\", "      \
  "\"boosts\".\"created_at\", \"boosts\".\"updated_at\""

// The column order of the Rust statements. `BoostRow::read` reads `kColumns` order, so this struct reads its own.
struct BoostCols {
  std::int64_t id;
  std::int64_t message_id;
  std::int64_t booster_id;
  std::string_view content;
  std::string_view created_at;
  std::string_view updated_at;
  static BoostCols read(db::RowReader& r) {
    return {r.i64(0), r.i64(1), r.i64(2), r.text(3), r.text(4), r.text(5)};
  }
};

const db::Query<BoostCols(std::int64_t, std::int64_t, std::int64_t)> kFindBy{
    "SELECT " CF_BOOST_COLUMNS
    " FROM \"boosts\" WHERE \"boosts\".\"message_id\" = ? AND \"boosts\".\"id\" = ? AND \"boosts\".\"booster_id\" = ? "
    "LIMIT 1"};
const db::Query<BoostCols(std::int64_t)> kOrdered{
    "SELECT " CF_BOOST_COLUMNS
    " FROM \"boosts\" WHERE \"boosts\".\"message_id\" = ? ORDER BY \"boosts\".\"created_at\" ASC"};
const db::Query<std::int64_t(std::int64_t, std::string_view, std::string_view, std::int64_t, std::string_view)>
    kInsert{
        "INSERT INTO \"boosts\" (\"booster_id\", \"content\", \"created_at\", \"message_id\", \"updated_at\") VALUES "
        "(?, ?, ?, ?, ?) RETURNING \"id\""};
const db::Query<void(std::int64_t)> kDelete{"DELETE FROM \"boosts\" WHERE \"boosts\".\"id\" = ?"};

Boost make(const BoostCols& row) {
  return {row.id, row.message_id, row.booster_id, std::string(row.content), std::string(row.created_at),
          std::string(row.updated_at)};
}

}  // namespace

Boost Boost::from_row(const BoostRow& row) {
  return {row.id, row.message_id, row.booster_id, std::string(row.content), std::string(row.created_at),
          std::string(row.updated_at)};
}

namespace boosts {

Result<std::optional<Boost>> find_by_message_and_booster(db::Connection& conn, Arena& arena, std::int64_t message_id,
                                                         std::int64_t id, std::int64_t booster_id) {
  auto row = conn.first(kFindBy, arena, message_id, id, booster_id);
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<Boost>{};
  return std::optional<Boost>(make(**row));
}

Result<std::vector<Boost>> for_message_ordered(db::Connection& conn, Arena& arena, std::int64_t message_id) {
  auto rows = conn.all(kOrdered, arena, message_id);
  if (!rows) return std::unexpected(rows.error());
  std::vector<Boost> out;
  out.reserve(rows->size());
  for (const BoostCols& row : *rows) out.push_back(make(row));
  return out;
}

Result<Boost> create(db::Tx& tx, std::int64_t message_id, std::int64_t booster_id, std::string_view content,
                     std::string_view plain_text) {
  Arena arena(256);
  const std::string now = tx.now_db();
  auto id = tx.conn().first(kInsert, arena, booster_id, content, now, message_id, now);
  if (!id) return std::unexpected(id.error());
  auto message = messages::find_by_id(tx.conn(), arena, message_id);
  if (!message) return std::unexpected(message.error());
  if (!*message) return fail(Errc::NotFound, "Couldn't find Message");
  if (auto touched = messages::touch(tx, **message, plain_text); !touched) return std::unexpected(touched.error());
  return Boost{**id, message_id, booster_id, std::string(content), now, now};
}

Status destroy(db::Tx& tx, const Boost& boost, std::string_view plain_text) {
  Arena arena(256);
  if (auto r = tx.conn().exec(kDelete, boost.id); !r) return std::unexpected(r.error());
  auto message = messages::find_by_id(tx.conn(), arena, boost.message_id);
  if (!message) return std::unexpected(message.error());
  if (!*message) return fail(Errc::NotFound, "Couldn't find Message");
  return messages::touch(tx, **message, plain_text);
}

}  // namespace boosts
}  // namespace campfire::models
