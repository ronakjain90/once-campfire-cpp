// Rails: app/models/account.rb, account/joinable.rb. Rust: crates/db/src/models/account.rs.
#include "models/account_admin.hpp"

#include <algorithm>

#include "compat/json.hpp"
#include "models/account.hpp"

namespace campfire::models::accounts {

namespace {

struct EditRow {
  std::int64_t id;
  std::string_view name;
  std::string_view join_code;
  std::optional<std::string_view> custom_styles;
  std::optional<std::string_view> settings;
  static EditRow read(db::RowReader& r) { return {r.i64(0), r.text(1), r.text(2), r.text_opt(3), r.text_opt(4)}; }
};
const db::Query<EditRow()> kFirst{
    "SELECT \"accounts\".\"id\", \"accounts\".\"name\", \"accounts\".\"join_code\", \"accounts\".\"custom_styles\", "
    "\"accounts\".\"settings\" FROM \"accounts\" ORDER BY \"accounts\".\"id\" ASC LIMIT 1"};
const db::Query<EditRow(std::int64_t)> kById{
    "SELECT \"accounts\".\"id\", \"accounts\".\"name\", \"accounts\".\"join_code\", \"accounts\".\"custom_styles\", "
    "\"accounts\".\"settings\" FROM \"accounts\" WHERE \"accounts\".\"id\" = ? LIMIT 1"};
const db::Query<void(std::string_view, std::optional<std::string_view>, std::optional<std::string_view>,
                     std::string_view, std::int64_t)>
    kUpdate{
        "UPDATE \"accounts\" SET \"name\" = ?, \"custom_styles\" = ?, \"settings\" = ?, \"updated_at\" = ? WHERE "
        "\"accounts\".\"id\" = ?"};
const db::Query<void(std::string_view, std::string_view, std::int64_t)> kJoinCode{
    "UPDATE \"accounts\" SET \"join_code\" = ?, \"updated_at\" = ? WHERE \"accounts\".\"id\" = ?"};
const db::Query<void(std::string_view, std::int64_t)> kTouchAccount{
    "UPDATE \"accounts\" SET \"updated_at\" = ? WHERE \"accounts\".\"id\" = ?"};
const db::Query<void(std::string_view, std::int64_t)> kTouchUser{
    "UPDATE \"users\" SET \"updated_at\" = ? WHERE \"users\".\"id\" = ?"};

constexpr std::string_view kRestrict = "restrict_room_creation_to_administrators";

// `has_json :settings`: the stored hash with the schema defaults merged in.
compat::json::Value read_settings(std::optional<std::string_view> raw) {
  compat::json::Value::Object object;
  if (raw) {
    if (auto parsed = compat::json::parse(*raw); parsed && parsed->is_object()) object = parsed->as_object();
  }
  const bool has_key = std::any_of(object.begin(), object.end(), [](const auto& m) { return m.first == kRestrict; });
  if (!has_key) object.emplace_back(std::string(kRestrict), compat::json::Value(false));
  return compat::json::Value(std::move(object));
}

// `ActiveModel::Type::Boolean#cast`: blank is nil, the false values are false, anything else is true.
compat::json::Value cast_boolean(std::string_view text) {
  if (text.empty()) return compat::json::Value(nullptr);
  for (const std::string_view f : {"0", "f", "F", "false", "FALSE", "off", "OFF"}) {
    if (text == f) return compat::json::Value(false);
  }
  return compat::json::Value(true);
}

bool same(const compat::json::Value& a, const compat::json::Value& b) {
  return compat::json::encode(a) == compat::json::encode(b);
}

}  // namespace

Result<std::optional<Edit>> first_edit(db::Connection& conn, Arena& arena) {
  auto row = conn.first(kFirst, arena);
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<Edit>{};
  return std::optional<Edit>(Edit{(*row)->id, std::string((*row)->name), std::string((*row)->join_code)});
}

Status update(db::Tx& tx, std::int64_t id, const Changes& changes) {
  Arena arena(1024);
  auto found = tx.conn().first(kById, arena, id);
  if (!found) return std::unexpected(found.error());
  if (!*found) return fail(Errc::NotFound, "Couldn't find Account");
  const EditRow& row = **found;
  std::string name(row.name);
  std::optional<std::string> styles = row.custom_styles ? std::optional<std::string>(std::string(*row.custom_styles))
                                                        : std::nullopt;
  std::optional<std::string> settings_json = row.settings ? std::optional<std::string>(std::string(*row.settings))
                                                          : std::nullopt;
  bool dirty = false;
  if (changes.name && *changes.name != name) {
    name = *changes.name;
    dirty = true;
  }
  if (changes.custom_styles && *changes.custom_styles != styles) {
    styles = *changes.custom_styles;
    dirty = true;
  }
  if (changes.settings) {
    const compat::json::Value original = read_settings(row.settings);
    compat::json::Value updated = original;
    for (const auto& [key, value] : *changes.settings) {
      if (key != kRestrict) return fail(Errc::Internal, "undefined method '" + key + "=' for account settings");
      for (auto& member : updated.as_object()) {
        if (member.first == key) member.second = cast_boolean(value);
      }
    }
    if (!settings_json || !same(updated, original)) {
      settings_json = compat::json::encode(updated);
      dirty = true;
    }
  }
  if (!dirty) return {};
  const std::string now = tx.now_db();
  const auto view = [](const std::optional<std::string>& s) {
    return s ? std::optional<std::string_view>(*s) : std::nullopt;
  };
  if (auto done = tx.conn().exec(kUpdate, name, view(styles), view(settings_json), now, id); !done) {
    return std::unexpected(done.error());
  }
  tx.changed(db::schema::Table::Accounts, id);
  return {};
}

Status reset_join_code(db::Tx& tx, std::int64_t id) {
  const std::string now = tx.now_db();
  if (auto done = tx.conn().exec(kJoinCode, generate_join_code(), now, id); !done) return std::unexpected(done.error());
  tx.changed(db::schema::Table::Accounts, id);
  return {};
}

Status touch_account(db::Tx& tx, std::int64_t id) {
  if (auto done = tx.conn().exec(kTouchAccount, tx.now_db(), id); !done) return std::unexpected(done.error());
  tx.changed(db::schema::Table::Accounts, id);
  return {};
}

Status touch_user(db::Tx& tx, std::int64_t id) {
  if (auto done = tx.conn().exec(kTouchUser, tx.now_db(), id); !done) return std::unexpected(done.error());
  tx.changed(db::schema::Table::Users, id);
  return {};
}

}  // namespace campfire::models::accounts
