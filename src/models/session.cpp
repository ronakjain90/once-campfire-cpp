// Rails: app/models/session.rb (has_secure_token, resume). Rust: crates/db/src/models/session.rs.
#include "models/session.hpp"

#include <sys/random.h>

#include <cstdlib>

#include "core/time_format.hpp"

namespace campfire::models {

namespace {

#define CF_SESSION_COLUMNS                                                                                           \
  "\"sessions\".\"id\", \"sessions\".\"created_at\", \"sessions\".\"ip_address\", \"sessions\".\"last_active_at\", " \
  "\"sessions\".\"token\", \"sessions\".\"updated_at\", \"sessions\".\"user_agent\", \"sessions\".\"user_id\""

const db::Query<db::schema::SessionRow(std::string_view)> kByToken{
    "SELECT " CF_SESSION_COLUMNS " FROM \"sessions\" WHERE \"sessions\".\"token\" = ? LIMIT 1"};

const db::Query<std::int64_t(std::string_view, std::optional<std::string_view>, std::string_view, std::string_view,
                             std::string_view, std::optional<std::string_view>, std::int64_t)>
    kInsert{
        "INSERT INTO \"sessions\" (\"created_at\", \"ip_address\", \"last_active_at\", \"token\", \"updated_at\", "
        "\"user_agent\", \"user_id\") VALUES (?, ?, ?, ?, ?, ?, ?) RETURNING \"id\""};

const db::Query<void(std::optional<std::string_view>, std::string_view, std::string_view,
                     std::optional<std::string_view>, std::int64_t)>
    kResume{
        "UPDATE \"sessions\" SET \"ip_address\" = ?, \"last_active_at\" = ?, \"updated_at\" = ?, \"user_agent\" = ? "
        "WHERE \"sessions\".\"id\" = ?"};

const db::Query<void(std::int64_t)> kDestroy{"DELETE FROM \"sessions\" WHERE \"sessions\".\"id\" = ?"};

}  // namespace

Session Session::from_row(const db::schema::SessionRow& row) {
  Session s;
  s.id = row.id;
  s.user_id = row.user_id;
  s.token = std::string(row.token);
  if (row.ip_address) s.ip_address = std::string(*row.ip_address);
  if (row.user_agent) s.user_agent = std::string(*row.user_agent);
  s.last_active_at = std::string(row.last_active_at);
  s.created_at = std::string(row.created_at);
  s.updated_at = std::string(row.updated_at);
  return s;
}

bool Session::needs_resume(Timestamp now) const {
  const auto last = parse_db(last_active_at);
  if (!last) return true;
  return *last < now.plus_seconds(-kActivityRefreshSeconds);
}

namespace sessions {

std::string generate_token() {
  static constexpr std::string_view kAlphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  std::string token;
  while (token.size() < 24) {
    unsigned char b[32];
    if (getrandom(b, sizeof b, 0) != static_cast<ssize_t>(sizeof b)) std::abort();
    for (const unsigned char byte : b) {
      if (byte < 232 && token.size() < 24) token.push_back(kAlphabet[byte % 58]);  // 232 = 58 * 4: no bias
    }
  }
  return token;
}

Result<std::optional<Session>> find_by_token(db::Connection& conn, Arena& arena, std::string_view token) {
  auto row = conn.first(kByToken, arena, token);
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<Session>{};
  return std::optional<Session>(Session::from_row(**row));
}

Result<Session> start(db::Tx& tx, std::int64_t user_id, std::optional<std::string_view> user_agent,
                      std::optional<std::string_view> ip_address) {
  const std::string now = tx.now_db();
  const std::string token = generate_token();
  Arena arena(1024);
  const auto id = tx.conn().first(kInsert, arena, now, ip_address, now, token, now, user_agent, user_id);
  if (!id) return std::unexpected(id.error());
  Session s;
  s.id = **id;
  s.user_id = user_id;
  s.token = token;
  if (ip_address) s.ip_address = std::string(*ip_address);
  if (user_agent) s.user_agent = std::string(*user_agent);
  s.last_active_at = s.created_at = s.updated_at = now;
  tx.changed(db::schema::Table::Sessions, s.id);
  return s;
}

Status resume(db::Tx& tx, Session& session, std::optional<std::string_view> user_agent,
              std::optional<std::string_view> ip_address) {
  if (!session.needs_resume(tx.now())) return {};
  const std::string now = tx.now_db();
  auto done = tx.conn().exec(kResume, ip_address, now, now, user_agent, session.id);
  if (!done) return std::unexpected(done.error());
  session.ip_address = ip_address ? std::optional<std::string>(std::string(*ip_address)) : std::nullopt;
  session.user_agent = user_agent ? std::optional<std::string>(std::string(*user_agent)) : std::nullopt;
  session.last_active_at = session.updated_at = now;
  tx.changed(db::schema::Table::Sessions, session.id);
  return {};
}

Status destroy(db::Tx& tx, const Session& session) {
  auto done = tx.conn().exec(kDestroy, session.id);
  if (!done) return std::unexpected(done.error());
  tx.changed(db::schema::Table::Sessions, session.id);
  return {};
}

}  // namespace sessions
}  // namespace campfire::models
