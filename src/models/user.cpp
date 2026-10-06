// Rails: app/models/user.rb and app/models/user/bot.rb. Rust: crates/db/src/models/user.rs.
#include "models/user.hpp"

#include "req/bcrypt.hpp"

namespace campfire::models {

namespace {

#define CF_USER_COLUMNS                                                                                  \
  "\"users\".\"id\", \"users\".\"bio\", \"users\".\"bot_token\", \"users\".\"created_at\", "             \
  "\"users\".\"email_address\", \"users\".\"name\", \"users\".\"password_digest\", \"users\".\"role\", " \
  "\"users\".\"status\", \"users\".\"updated_at\""

const db::Query<db::schema::UserRow(std::int64_t)> kById{"SELECT " CF_USER_COLUMNS
                                                         " FROM \"users\" WHERE \"users\".\"id\" = ? LIMIT 1"};

const db::Query<db::schema::UserRow(std::string_view)> kActiveByEmail{
    "SELECT " CF_USER_COLUMNS
    " FROM \"users\" WHERE \"users\".\"status\" = 0 AND \"users\".\"email_address\" = ? LIMIT 1"};

const db::Query<db::schema::UserRow(std::string_view, std::string_view)> kBot{
    "SELECT " CF_USER_COLUMNS
    " FROM \"users\" WHERE \"users\".\"status\" = 0 AND \"users\".\"role\" = 2 AND \"users\".\"id\" = ? AND "
    "\"users\".\"bot_token\" = ? LIMIT 1"};

const db::Query<std::int64_t(std::optional<std::string_view>, std::optional<std::string_view>, std::string_view,
                             std::optional<std::string_view>, std::string_view, std::optional<std::string_view>,
                             std::int64_t, std::int64_t, std::string_view)>
    kInsert{
        "INSERT INTO \"users\" (\"bio\", \"bot_token\", \"created_at\", \"email_address\", \"name\", "
        "\"password_digest\", \"role\", \"status\", \"updated_at\") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) RETURNING "
        "\"id\""};

// `Membership.insert_all(Rooms::Open.pluck(:id).collect { ... })`: one row for each open room. The times are the
// time of the database, as the Rust port writes them.
const db::Query<void(std::int64_t)> kGrantOpenRooms{
    "INSERT INTO \"memberships\" (\"created_at\",\"room_id\",\"updated_at\",\"user_id\") "
    "SELECT STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW'), \"rooms\".\"id\", STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW'), ? "
    "FROM \"rooms\" WHERE \"rooms\".\"type\" = 'Rooms::Open' ON CONFLICT DO NOTHING"};

const db::Query<std::int64_t()> kAny{"SELECT 1 AS one FROM \"users\" LIMIT 1"};

struct OwnerRow {
  std::string_view name;
  std::optional<std::string_view> email;
  static OwnerRow read(db::RowReader& r) { return {r.text(0), r.text_opt(1)}; }
};
const db::Query<OwnerRow()> kOwner{
    "SELECT \"users\".\"name\", \"users\".\"email_address\" FROM \"users\" WHERE \"users\".\"role\" = 1 "
    "ORDER BY \"users\".\"id\" ASC LIMIT 1"};

const db::Query<db::schema::UserRow(std::int64_t)> kOfRoom{
    "SELECT " CF_USER_COLUMNS
    " FROM \"users\" INNER JOIN \"memberships\" ON \"users\".\"id\" = \"memberships\".\"user_id\" WHERE "
    "\"memberships\".\"room_id\" = ?"};

// The user ids that share a direct room with a user (`Membership.where(room_id: directs).pluck(:user_id).uniq`).
const db::Query<std::int64_t(std::int64_t)> kDirectNeighbours{
    "SELECT DISTINCT \"memberships\".\"user_id\" FROM \"memberships\" WHERE \"memberships\".\"room_id\" IN "
    "(SELECT \"rooms\".\"id\" FROM \"rooms\" INNER JOIN \"memberships\" ON \"rooms\".\"id\" = "
    "\"memberships\".\"room_id\" WHERE \"memberships\".\"user_id\" = ? AND \"rooms\".\"type\" = "
    "'Rooms::Direct')"};

// `User.active.where.not(id: ids).order(:created_at).limit(n)`: the ids come as a JSON array.
const db::Query<db::schema::UserRow(std::string_view, std::int64_t)> kPlaceholders{
    "SELECT " CF_USER_COLUMNS
    " FROM \"users\" WHERE \"users\".\"status\" = 0 AND \"users\".\"id\" NOT IN (SELECT value FROM "
    "json_each(?)) ORDER BY \"users\".\"created_at\" ASC LIMIT ?"};

const db::Query<db::schema::UserRow()> kActiveOrdered{
    "SELECT " CF_USER_COLUMNS " FROM \"users\" WHERE \"users\".\"status\" = 0 ORDER BY LOWER(name)"};

const db::Query<std::int64_t(std::string_view)> kExistingIds{
    "SELECT \"users\".\"id\" FROM \"users\" WHERE \"users\".\"id\" IN (SELECT value FROM json_each(?)) ORDER BY "
    "\"users\".\"id\""};

// `BCrypt::Password.create("dummy", cost: 12)`: same cost as real digests (Rust: DUMMY_DIGEST).
constexpr std::string_view kDummyDigest = "$2a$12$FiKmSp4UhLvSB4Sd/ZUjQunyKP6.NjDRHdr5LnKUVk.BUn4Mq12WS";

Result<std::optional<User>> wrap(Result<std::optional<db::schema::UserRow>> row) {
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<User>{};
  return std::optional<User>(User::from_row(**row));
}

}  // namespace

User User::from_row(const db::schema::UserRow& row) {
  User u;
  u.id = row.id;
  if (row.bio) u.bio = std::string(*row.bio);
  if (row.bot_token) u.bot_token = std::string(*row.bot_token);
  u.created_at = std::string(row.created_at);
  if (row.email_address) u.email_address = std::string(*row.email_address);
  u.name = std::string(row.name);
  if (row.password_digest) u.password_digest = std::string(*row.password_digest);
  u.role = row.role;
  u.status = row.status;
  u.updated_at = std::string(row.updated_at);
  return u;
}

bool User::authenticate(std::string_view password) const {
  if (!password_digest || password_digest->empty()) return false;
  return req::bcrypt::verify_password(password, *password_digest);
}

namespace users {

Result<User> create(db::Tx& tx, const NewUser& attributes) {
  const std::string now = tx.now_db();
  const auto view = [](const std::optional<std::string>& s) {
    return s ? std::optional<std::string_view>(*s) : std::nullopt;
  };
  Arena arena(256);
  auto id =
      tx.conn().first(kInsert, arena, std::nullopt, std::nullopt, now, view(attributes.email_address), attributes.name,
                      view(attributes.password_digest), static_cast<std::int64_t>(attributes.role), kStatusActive, now);
  if (!id) return std::unexpected(id.error());
  if (auto granted = tx.conn().exec(kGrantOpenRooms, **id); !granted) return std::unexpected(granted.error());
  User u;
  u.id = **id;
  u.created_at = u.updated_at = now;
  u.email_address = attributes.email_address;
  u.name = attributes.name;
  u.password_digest = attributes.password_digest;
  u.role = static_cast<std::int64_t>(attributes.role);
  u.status = kStatusActive;
  tx.changed(db::schema::Table::Users, u.id);
  tx.changed(db::schema::Table::Memberships, u.id);
  return u;
}

Result<std::optional<User>> find_by_id(db::Connection& conn, Arena& arena, std::int64_t id) {
  return wrap(conn.first(kById, arena, id));
}

Result<std::optional<User>> find_active_by_email_address(db::Connection& conn, Arena& arena,
                                                         std::string_view email_address) {
  return wrap(conn.first(kActiveByEmail, arena, email_address));
}

Result<std::optional<User>> authenticate_bot(db::Connection& conn, Arena& arena, std::string_view bot_key) {
  // Ruby `split("-")` drops trailing empty fields: a key without a token finds nothing.
  const std::size_t dash = bot_key.find('-');
  if (dash == std::string_view::npos) return std::optional<User>{};
  const std::string_view id_text = bot_key.substr(0, dash);
  std::string_view token = bot_key.substr(dash + 1);
  if (const std::size_t next = token.find('-'); next != std::string_view::npos) token = token.substr(0, next);
  if (token.empty()) return std::optional<User>{};
  return wrap(conn.first(kBot, arena, id_text, token));
}

std::optional<User> authenticated(std::optional<User> candidate, std::string_view password) {
  if (password.empty()) return std::nullopt;
  if (!candidate) {
    // authenticate_by hashes anyway: a missing account takes as long as a wrong password.
    [[maybe_unused]] const bool ignored = req::bcrypt::verify_password(password, kDummyDigest);
    return std::nullopt;
  }
  if (!candidate->authenticate(password)) return std::nullopt;
  return candidate;
}

Result<std::vector<User>> of_room(db::Connection& conn, Arena& arena, std::int64_t room_id) {
  auto rows = conn.all(kOfRoom, arena, room_id);
  if (!rows) return std::unexpected(rows.error());
  std::vector<User> out;
  out.reserve(rows->size());
  for (const db::schema::UserRow& row : *rows) out.push_back(User::from_row(row));
  return out;
}

Result<std::vector<User>> direct_placeholders(db::Connection& conn, Arena& arena, std::int64_t user_id) {
  auto neighbours = conn.all(kDirectNeighbours, arena, user_id);
  if (!neighbours) return std::unexpected(neighbours.error());
  // `exclude_user_ids.including(Current.user.id)` appends the id even when it is there, and the limit counts it.
  std::string ids = "[";
  for (const std::int64_t id : *neighbours) ids += std::to_string(id) + ",";
  ids += std::to_string(user_id) + "]";
  const std::int64_t excluded = static_cast<std::int64_t>(neighbours->size()) + 1;
  const std::int64_t limit = std::max<std::int64_t>(kDirectPlaceholders - excluded, 0);
  auto rows = conn.all(kPlaceholders, arena, ids, limit);
  if (!rows) return std::unexpected(rows.error());
  std::vector<User> out;
  out.reserve(rows->size());
  for (const db::schema::UserRow& row : *rows) out.push_back(User::from_row(row));
  return out;
}

Result<std::vector<User>> active_ordered(db::Connection& conn, Arena& arena) {
  auto rows = conn.all(kActiveOrdered, arena);
  if (!rows) return std::unexpected(rows.error());
  std::vector<User> out;
  out.reserve(rows->size());
  for (const db::schema::UserRow& row : *rows) out.push_back(User::from_row(row));
  return out;
}

Result<std::vector<std::int64_t>> existing_ids(db::Connection& conn, Arena& arena, std::span<const std::int64_t> ids) {
  std::string json = "[";
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (i > 0) json += ',';
    json += std::to_string(ids[i]);
  }
  json += ']';
  auto rows = conn.all(kExistingIds, arena, json);
  if (!rows) return std::unexpected(rows.error());
  return std::vector<std::int64_t>(rows->begin(), rows->end());
}

Result<bool> none(db::Connection& conn, Arena& arena) {
  auto row = conn.first(kAny, arena);
  if (!row) return std::unexpected(row.error());
  return !row->has_value();
}

Result<std::optional<Owner>> first_administrator(db::Connection& conn, Arena& arena) {
  auto row = conn.first(kOwner, arena);
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<Owner>{};
  return std::optional<Owner>(Owner{std::string((*row)->name), std::string((*row)->email.value_or(""))});
}

}  // namespace users
}  // namespace campfire::models
