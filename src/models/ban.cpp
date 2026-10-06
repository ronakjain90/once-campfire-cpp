// Rails: app/models/ban.rb. Rust: crates/db/src/models/ban.rs.
#include "models/ban.hpp"

namespace campfire::models::bans {

namespace {
const db::Query<std::int64_t(std::string_view)> kBanned{
    "SELECT 1 AS one FROM \"bans\" WHERE \"bans\".\"ip_address\" = ? LIMIT 1"};
}

Result<bool> banned(db::Connection& conn, Arena& arena, std::string_view ip_address) {
  auto row = conn.first(kBanned, arena, ip_address);
  if (!row) return std::unexpected(row.error());
  return row->has_value();
}

}  // namespace campfire::models::bans
