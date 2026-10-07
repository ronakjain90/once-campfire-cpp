// Rails: app/models/account.rb. Rust: crates/db/src/models/account.rs, presenters/view_context.rs.
#include "models/account.hpp"

#include <sys/random.h>

#include <cstdlib>

#include "compat/json.hpp"

namespace campfire::models {

namespace {

struct AccountRow {
  std::int64_t id;
  std::string_view name;
  std::optional<std::string_view> custom_styles;
  std::string_view updated_at;
  std::optional<std::string_view> settings;
  bool has_logo;
  static AccountRow read(db::RowReader& r) {
    return {r.i64(0), r.text(1), r.text_opt(2), r.text(3), r.text_opt(4), r.boolean(5)};
  }
};

// The logo flag comes with the row: one statement, not two.
const db::Query<AccountRow()> kFirst{
    "SELECT \"accounts\".\"id\", \"accounts\".\"name\", \"accounts\".\"custom_styles\", \"accounts\".\"updated_at\", "
    "\"accounts\".\"settings\", EXISTS (SELECT 1 FROM active_storage_blobs b JOIN active_storage_attachments a ON "
    "a.blob_id = b.id WHERE a.record_type = 'Account' AND a.record_id = \"accounts\".\"id\" AND a.name = 'logo') "
    "FROM \"accounts\" ORDER BY \"accounts\".\"id\" ASC LIMIT 1"};

const db::Query<std::int64_t()> kAny{"SELECT 1 AS one FROM \"accounts\" LIMIT 1"};

struct JoinCodeRow {
  std::string_view join_code;
  static JoinCodeRow read(db::RowReader& r) { return {r.text(0)}; }
};
const db::Query<JoinCodeRow()> kJoinCode{
    "SELECT \"accounts\".\"join_code\" FROM \"accounts\" ORDER BY \"accounts\".\"id\" ASC LIMIT 1"};

const db::Query<std::int64_t(std::string_view, std::string_view, std::string_view, std::string_view, std::string_view)>
    kInsert{
        "INSERT INTO \"accounts\" (\"created_at\", \"custom_styles\", \"join_code\", \"name\", \"settings\", "
        "\"singleton_guard\", \"updated_at\") VALUES (?, NULL, ?, ?, ?, 0, ?) RETURNING \"id\""};
bool parse_restrict_room_creation(std::string_view settings_text);
// `restrict_room_creation_to_administrators?` of `has_json :settings`: `present?` of the stored value.
bool restrict_room_creation(std::optional<std::string_view> settings) {
  if (!settings) return false;
  // The settings text seldom changes: each worker thread keeps the last answer.
  thread_local std::string last_text;
  thread_local bool last_answer = false;
  thread_local bool have_last = false;
  if (have_last && last_text == *settings) return last_answer;
  const bool answer = parse_restrict_room_creation(*settings);
  last_text.assign(*settings);
  last_answer = answer;
  have_last = true;
  return answer;
}

bool parse_restrict_room_creation(std::string_view settings_text) {
  const auto json = compat::json::parse(settings_text);
  if (!json || !json->is_object()) return false;
  const compat::json::Value* value = json->find("restrict_room_creation_to_administrators");
  if (value == nullptr || value->is_null()) return false;
  if (value->is_bool()) return value->as_bool();
  if (const std::string* text = value->get_string()) return text->find_first_not_of(" \t\n\v\f\r") != std::string::npos;
  if (value->is_array()) return !value->as_array().empty();
  if (value->is_object()) return !value->as_object().empty();
  return true;
}

}  // namespace

namespace accounts {

std::string generate_join_code() {
  static constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  std::string raw;
  while (raw.size() < 12) {
    unsigned char b[32];
    if (getrandom(b, sizeof b, 0) != static_cast<ssize_t>(sizeof b)) std::abort();
    for (const unsigned char byte : b) {
      if (byte < 248 && raw.size() < 12) raw.push_back(kAlphabet[byte % 62]);  // 248 = 62 * 4: no bias
    }
  }
  return raw.substr(0, 4) + "-" + raw.substr(4, 4) + "-" + raw.substr(8, 4);
}

Result<bool> any(db::Connection& conn, Arena& arena) {
  auto row = conn.first(kAny, arena);
  if (!row) return std::unexpected(row.error());
  return row->has_value();
}

Result<std::optional<std::string>> first_join_code(db::Connection& conn, Arena& arena) {
  auto row = conn.first(kJoinCode, arena);
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<std::string>{};
  return std::optional<std::string>(std::string((*row)->join_code));
}

Result<std::int64_t> create(db::Tx& tx, std::string_view name) {
  const std::string now = tx.now_db();
  const std::string join_code = generate_join_code();
  Arena arena(256);
  // `has_json :settings, restrict_room_creation_to_administrators: false`: the defaults are written out.
  auto id = tx.conn().first(kInsert, arena, now, join_code, name,
                            "{\"restrict_room_creation_to_administrators\":false}", now);
  if (!id) return std::unexpected(id.error());
  tx.changed(db::schema::Table::Accounts, **id);
  return **id;
}

Result<std::optional<Account>> first(db::Connection& conn, Arena& arena) {
  auto row = conn.first(kFirst, arena);
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<Account>{};
  Account a;
  a.id = (*row)->id;
  a.name = std::string((*row)->name);
  if ((*row)->custom_styles) a.custom_styles = std::string(*(*row)->custom_styles);
  a.updated_at = std::string((*row)->updated_at);
  a.restrict_room_creation_to_administrators = restrict_room_creation((*row)->settings);
  a.has_logo = (*row)->has_logo;
  return std::optional<Account>(std::move(a));
}

}  // namespace accounts
}  // namespace campfire::models
