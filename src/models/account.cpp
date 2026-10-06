// Rails: app/models/account.rb. Rust: crates/db/src/models/account.rs, presenters/view_context.rs.
#include "models/account.hpp"

namespace campfire::models {

namespace {

struct AccountRow {
  std::int64_t id;
  std::string_view name;
  std::optional<std::string_view> custom_styles;
  std::string_view updated_at;
  static AccountRow read(db::RowReader& r) { return {r.i64(0), r.text(1), r.text_opt(2), r.text(3)}; }
};

const db::Query<AccountRow()> kFirst{
    "SELECT \"accounts\".\"id\", \"accounts\".\"name\", \"accounts\".\"custom_styles\", \"accounts\".\"updated_at\" "
    "FROM \"accounts\" ORDER BY \"accounts\".\"id\" ASC LIMIT 1"};

const db::Query<std::int64_t(std::int64_t)> kLogo{
    "SELECT 1 AS one FROM active_storage_blobs b JOIN active_storage_attachments a ON a.blob_id = b.id WHERE "
    "a.record_type = 'Account' AND a.record_id = ? AND a.name = 'logo' ORDER BY a.id LIMIT 1"};

}  // namespace

namespace accounts {

Result<std::optional<Account>> first(db::Connection& conn, Arena& arena) {
  auto row = conn.first(kFirst, arena);
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<Account>{};
  Account a;
  a.id = (*row)->id;
  a.name = std::string((*row)->name);
  if ((*row)->custom_styles) a.custom_styles = std::string(*(*row)->custom_styles);
  a.updated_at = std::string((*row)->updated_at);
  auto logo = conn.first(kLogo, arena, a.id);
  if (!logo) return std::unexpected(logo.error());
  a.has_logo = logo->has_value();
  return std::optional<Account>(std::move(a));
}

}  // namespace accounts
}  // namespace campfire::models
