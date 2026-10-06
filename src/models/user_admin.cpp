// Rails: app/models/user.rb, user/bot.rb, user/bannable.rb, ban.rb, webhook.rb. Rust: crates/db/src/models/user.rs,
// ban.rs, webhook.rs.
#include "models/user_admin.hpp"

#include <arpa/inet.h>
#include <sys/random.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

#include "models/hooks.hpp"

namespace campfire::models {

namespace {

using db::schema::UserRow;

#define CF_UA_USER "SELECT " CF_USER_COLUMNS_A4 " FROM \"users\" "
#define CF_USER_COLUMNS_A4                                                                               \
  "\"users\".\"id\", \"users\".\"bio\", \"users\".\"bot_token\", \"users\".\"created_at\", "             \
  "\"users\".\"email_address\", \"users\".\"name\", \"users\".\"password_digest\", \"users\".\"role\", " \
  "\"users\".\"status\", \"users\".\"updated_at\""

const db::Query<UserRow(std::int64_t)> kById{CF_UA_USER "WHERE \"users\".\"id\" = ? LIMIT 1"};
const db::Query<UserRow(std::int64_t)> kActiveById{CF_UA_USER
                                                   "WHERE \"users\".\"status\" = 0 AND \"users\".\"id\" = ? LIMIT 1"};
const db::Query<UserRow(std::int64_t)> kActiveBotById{
    CF_UA_USER "WHERE \"users\".\"status\" = 0 AND \"users\".\"role\" = 2 AND \"users\".\"id\" = ? LIMIT 1"};
const db::Query<UserRow()> kAccountUsers{
    CF_UA_USER "WHERE \"users\".\"status\" = 0 AND \"users\".\"role\" != 2 ORDER BY LOWER(name)"};
const db::Query<UserRow()> kAccountUsersWithBanned{
    CF_UA_USER "WHERE \"users\".\"status\" IN (0, 2) AND \"users\".\"role\" != 2 ORDER BY LOWER(name)"};
const db::Query<UserRow()> kActiveBots{
    CF_UA_USER "WHERE \"users\".\"status\" = 0 AND \"users\".\"role\" = 2 ORDER BY LOWER(name)"};

struct UrlRow {
  std::string_view url;
  static UrlRow read(db::RowReader& r) { return {r.text(0)}; }
};
const db::Query<UrlRow(std::int64_t)> kWebhookUrl{
    "SELECT \"webhooks\".\"url\" FROM \"webhooks\" WHERE \"webhooks\".\"user_id\" = ? LIMIT 1"};

struct WebhookRow {
  std::int64_t id;
  std::optional<std::string_view> url;
  static WebhookRow read(db::RowReader& r) { return {r.i64(0), r.text_opt(1)}; }
};
const db::Query<WebhookRow(std::int64_t)> kWebhook{
    "SELECT \"webhooks\".\"id\", \"webhooks\".\"url\" FROM \"webhooks\" WHERE \"webhooks\".\"user_id\" = ? LIMIT 1"};
const db::Query<std::int64_t(std::string_view, std::string_view, std::optional<std::string_view>, std::int64_t)>
    kWebhookInsert{
        "INSERT INTO \"webhooks\" (\"created_at\", \"updated_at\", \"url\", \"user_id\") VALUES (?, ?, ?, ?) RETURNING "
        "\"id\""};
const db::Query<void(std::string_view, std::string_view, std::int64_t)> kWebhookUpdate{
    "UPDATE \"webhooks\" SET \"updated_at\" = ?, \"url\" = ? WHERE \"webhooks\".\"id\" = ?"};
const db::Query<void(std::int64_t)> kWebhookDelete{"DELETE FROM \"webhooks\" WHERE \"webhooks\".\"id\" = ?"};

struct BotRoomRow {
  std::int64_t id;
  std::string_view name;
  static BotRoomRow read(db::RowReader& r) { return {r.i64(0), r.text(1)}; }
};
// `bot.rooms.without_directs.ordered`: `Room.ordered` is `order("LOWER(name)")`.
const db::Query<BotRoomRow(std::int64_t)> kBotRooms{
    "SELECT \"rooms\".\"id\", COALESCE(\"rooms\".\"name\", '') FROM \"rooms\" INNER JOIN \"memberships\" ON "
    "\"rooms\".\"id\" = \"memberships\".\"room_id\" WHERE \"memberships\".\"user_id\" = ? AND \"rooms\".\"type\" != "
    "'Rooms::Direct' ORDER BY LOWER(name)"};

struct ProfileRow {
  std::int64_t room_id;
  std::string_view type;
  std::optional<std::string_view> name;
  std::optional<std::string_view> involvement;
  static ProfileRow read(db::RowReader& r) { return {r.i64(0), r.text(1), r.text_opt(2), r.text_opt(3)}; }
};
const db::Query<ProfileRow(std::int64_t)> kProfileMemberships{
    "SELECT \"rooms\".\"id\", \"rooms\".\"type\", \"rooms\".\"name\", \"memberships\".\"involvement\" FROM "
    "\"memberships\" "
    "INNER JOIN \"rooms\" ON \"rooms\".\"id\" = \"memberships\".\"room_id\" WHERE \"memberships\".\"user_id\" = ? "
    "ORDER BY "
    "LOWER(rooms.name)"};

const db::Query<void(std::string_view, std::optional<std::string_view>, std::optional<std::string_view>,
                     std::optional<std::string_view>, std::optional<std::string_view>, std::int64_t, std::int64_t,
                     std::string_view, std::int64_t)>
    kUpdate{
        "UPDATE \"users\" SET \"name\" = ?, \"email_address\" = ?, \"password_digest\" = ?, \"bio\" = ?, "
        "\"bot_token\" = ?, \"role\" = ?, \"status\" = ?, \"updated_at\" = ? WHERE \"users\".\"id\" = ?"};

const db::Query<void(std::int64_t)> kDeleteMemberships{
    "DELETE FROM \"memberships\" WHERE (\"memberships\".\"id\") IN (SELECT \"memberships\".\"id\" FROM "
    "\"memberships\" INNER JOIN \"rooms\" AS \"room\" ON \"room\".\"id\" = \"memberships\".\"room_id\" WHERE "
    "\"memberships\".\"user_id\" = ? AND \"room\".\"type\" != 'Rooms::Direct')"};
const db::Query<void(std::int64_t)> kDeletePush{
    "DELETE FROM \"push_subscriptions\" WHERE \"push_subscriptions\".\"user_id\" = ?"};
const db::Query<void(std::int64_t)> kDeleteSearches{"DELETE FROM \"searches\" WHERE \"searches\".\"user_id\" = ?"};
const db::Query<void(std::int64_t)> kDeleteSessions{"DELETE FROM \"sessions\" WHERE \"sessions\".\"user_id\" = ?"};
const db::Query<void(std::int64_t)> kDeleteBans{"DELETE FROM \"bans\" WHERE \"bans\".\"user_id\" = ?"};

struct IpRow {
  std::optional<std::string_view> ip;
  static IpRow read(db::RowReader& r) { return {r.text_opt(0)}; }
};
const db::Query<IpRow(std::int64_t)> kSessionIps{
    "SELECT \"sessions\".\"ip_address\" FROM \"sessions\" WHERE \"sessions\".\"user_id\" = ?"};
const db::Query<std::int64_t(std::string_view, std::string_view, std::string_view, std::int64_t)> kBanInsert{
    "INSERT INTO \"bans\" (\"created_at\", \"ip_address\", \"updated_at\", \"user_id\") VALUES (?, ?, ?, ?) RETURNING "
    "\"id\""};

bool blank(std::string_view text) {
  return text.find_first_not_of(" \t\n\v\f\r") == std::string_view::npos;
}

Result<std::optional<User>> wrap(Result<std::optional<UserRow>> row) {
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<User>{};
  return std::optional<User>(User::from_row(**row));
}

Result<std::vector<User>> wrap_all(Result<std::pmr::vector<UserRow>> rows) {
  if (!rows) return std::unexpected(rows.error());
  std::vector<User> out;
  out.reserve(rows->size());
  for (const UserRow& row : *rows) out.push_back(User::from_row(row));
  return out;
}

std::optional<std::string_view> view(const std::optional<std::string>& s) {
  return s ? std::optional<std::string_view>(*s) : std::nullopt;
}

void random_fill(unsigned char* out, std::size_t n) {
  if (getrandom(out, n, 0) != static_cast<ssize_t>(n)) std::abort();
}

std::string uuid() {
  std::array<unsigned char, 16> b{};
  random_fill(b.data(), b.size());
  b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);
  b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < b.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
    out.push_back(kHex[b[i] >> 4]);
    out.push_back(kHex[b[i] & 15]);
  }
  return out;
}

// The address that `IPAddr.new` reads: a `/prefix` masks it, `[::1]` is allowed.
struct Parsed {
  bool v6 = false;
  std::array<unsigned char, 16> bytes{};
};

std::optional<Parsed> parse_ipaddr(std::string_view text) {
  std::string address(text);
  std::optional<int> prefix;
  if (const std::size_t slash = address.find('/'); slash != std::string::npos) {
    const std::string digits = address.substr(slash + 1);
    if (digits.empty() || digits.size() > 3 ||
        !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
      return std::nullopt;
    }
    prefix = std::stoi(digits);
    address.resize(slash);
  }
  if (address.size() >= 2 && address.front() == '[' && address.back() == ']')
    address = address.substr(1, address.size() - 2);
  Parsed parsed;
  in_addr v4{};
  in6_addr v6{};
  if (inet_pton(AF_INET, address.c_str(), &v4) == 1) {
    std::memcpy(parsed.bytes.data(), &v4, 4);
  } else if (inet_pton(AF_INET6, address.c_str(), &v6) == 1) {
    parsed.v6 = true;
    std::memcpy(parsed.bytes.data(), &v6, 16);
  } else {
    return std::nullopt;
  }
  const int bits = parsed.v6 ? 128 : 32;
  if (prefix) {
    if (*prefix > bits) return std::nullopt;
    for (int i = 0; i < bits / 8; ++i) {
      const int keep = std::clamp(*prefix - i * 8, 0, 8);
      parsed.bytes[static_cast<std::size_t>(i)] &= static_cast<unsigned char>(keep == 0 ? 0 : 0xFF << (8 - keep));
    }
  }
  return parsed;
}

bool internal_v4(const unsigned char* a) {
  const std::uint32_t v = (std::uint32_t{a[0]} << 24) | (std::uint32_t{a[1]} << 16) | (std::uint32_t{a[2]} << 8) | a[3];
  return (v & 0xff000000U) == 0x7f000000U || (v & 0xff000000U) == 0x0a000000U || (v & 0xfff00000U) == 0xac100000U ||
         (v & 0xffff0000U) == 0xc0a80000U || (v & 0xffff0000U) == 0xa9fe0000U;
}

bool internal_address(const Parsed& p) {
  if (!p.v6) return internal_v4(p.bytes.data());
  static constexpr std::array<unsigned char, 16> kLoopback = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  if (p.bytes == kLoopback) return true;
  if ((p.bytes[0] & 0xFE) == 0xFC) return true;                        // fc00::/7
  if (p.bytes[0] == 0xFE && (p.bytes[1] & 0xC0) == 0x80) return true;  // fe80::/10
  // Ruby only checks the `ffff` bits of an IPv4 mapped address.
  if (p.bytes[10] == 0xFF && p.bytes[11] == 0xFF) return internal_v4(p.bytes.data() + 12);
  return false;
}

}  // namespace

namespace bans {

std::optional<std::string> validate(std::string_view ip_address) {
  const auto parsed = parse_ipaddr(ip_address);
  if (!parsed) return "Ip address is not a valid IP address";
  if (internal_address(*parsed)) return "Ip address cannot be a private or internal IP address";
  return std::nullopt;
}

}  // namespace bans

namespace users {

std::string generate_bot_token() {
  static constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  std::string out;
  while (out.size() < 12) {
    std::array<unsigned char, 32> b{};
    random_fill(b.data(), b.size());
    for (const unsigned char byte : b) {
      if (byte < 248 && out.size() < 12) out.push_back(kAlphabet[byte % 62]);
    }
  }
  return out;
}

Result<std::optional<User>> find_active(db::Connection& conn, Arena& arena, std::int64_t id) {
  return wrap(conn.first(kActiveById, arena, id));
}

Result<std::optional<User>> find_active_bot(db::Connection& conn, Arena& arena, std::int64_t id) {
  return wrap(conn.first(kActiveBotById, arena, id));
}

Result<std::vector<User>> account_users(db::Connection& conn, Arena& arena, bool with_banned) {
  return wrap_all(with_banned ? conn.all(kAccountUsersWithBanned, arena) : conn.all(kAccountUsers, arena));
}

Result<std::vector<User>> active_bots_ordered(db::Connection& conn, Arena& arena) {
  return wrap_all(conn.all(kActiveBots, arena));
}

Result<std::vector<ProfileMembership>> profile_memberships(db::Connection& conn, Arena& arena, std::int64_t user_id) {
  auto rows = conn.all(kProfileMemberships, arena, user_id);
  if (!rows) return std::unexpected(rows.error());
  std::vector<ProfileMembership> out;
  for (const ProfileRow& row : *rows) {
    ProfileMembership m;
    m.room_id = row.room_id;
    m.room_type = std::string(row.type);
    if (row.name) m.room_name = std::string(*row.name);
    if (row.involvement) m.involvement = std::string(*row.involvement);
    out.push_back(std::move(m));
  }
  return out;
}

Result<std::optional<std::string>> webhook_url(db::Connection& conn, Arena& arena, std::int64_t user_id) {
  auto row = conn.first(kWebhookUrl, arena, user_id);
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<std::string>{};
  return std::optional<std::string>(std::string((*row)->url));
}

Result<std::vector<BotRoom>> bot_rooms(db::Connection& conn, Arena& arena, std::int64_t user_id) {
  auto rows = conn.all(kBotRooms, arena, user_id);
  if (!rows) return std::unexpected(rows.error());
  std::vector<BotRoom> out;
  for (const BotRoomRow& row : *rows) out.push_back({row.id, std::string(row.name)});
  return out;
}

Result<User> update(db::Tx& tx, std::int64_t id, const UserChanges& changes) {
  Arena arena(1024);
  auto current = wrap(tx.conn().first(kById, arena, id));
  if (!current) return std::unexpected(current.error());
  if (!*current) return fail(Errc::NotFound, "Couldn't find User");
  User user = std::move(**current);
  bool dirty = false;
  if (changes.name && *changes.name != user.name) {
    user.name = *changes.name;
    dirty = true;
  }
  if (changes.email_address && *changes.email_address != user.email_address) {
    user.email_address = *changes.email_address;
    dirty = true;
  }
  if (changes.password_digest) {
    user.password_digest = *changes.password_digest;
    dirty = true;
  }
  if (changes.role && *changes.role != user.role) {
    user.role = *changes.role;
    dirty = true;
  }
  if (changes.status && *changes.status != user.status) {
    user.status = *changes.status;
    dirty = true;
  }
  if (changes.bio && *changes.bio != user.bio) {
    user.bio = *changes.bio;
    dirty = true;
  }
  if (changes.bot_token && *changes.bot_token != user.bot_token) {
    user.bot_token = *changes.bot_token;
    dirty = true;
  }
  if (!dirty) return user;
  user.updated_at = tx.now_db();
  auto done = tx.conn().exec(kUpdate, user.name, view(user.email_address), view(user.password_digest), view(user.bio),
                             view(user.bot_token), user.role, user.status, user.updated_at, user.id);
  if (!done) return std::unexpected(done.error());
  tx.changed(db::schema::Table::Users, user.id);
  return user;
}

namespace {

// `close_remote_connections`: after the commit, as the Rust port does.
void close_remote_connections(db::Tx& tx, std::int64_t id, bool reconnect) {
  tx.after_commit([id, reconnect] { hooks::disconnect_user(id, reconnect); });
}

}  // namespace

Status deactivate(db::Tx& tx, std::int64_t id) {
  Arena arena(1024);
  auto found = wrap(tx.conn().first(kById, arena, id));
  if (!found) return std::unexpected(found.error());
  if (!*found) return fail(Errc::NotFound, "Couldn't find User");
  close_remote_connections(tx, id, false);
  for (const auto* query : {&kDeleteMemberships, &kDeletePush, &kDeleteSearches, &kDeleteSessions}) {
    if (auto done = tx.conn().exec(*query, id); !done) return std::unexpected(done.error());
  }
  tx.changed(db::schema::Table::Memberships, id);
  tx.changed(db::schema::Table::PushSubscriptions, id);
  tx.changed(db::schema::Table::Searches, id);
  UserChanges changes;
  changes.status = kStatusDeactivated;
  // `email_address&.gsub(/@/, "-deactivated-#{SecureRandom.uuid}@")`
  if (const auto& email = (*found)->email_address) {
    const std::string tag = "-deactivated-" + uuid() + "@";
    std::string replaced;
    for (const char c : *email) {
      if (c == '@')
        replaced += tag;
      else
        replaced.push_back(c);
    }
    changes.email_address = std::optional<std::string>(std::move(replaced));
  }
  auto updated = update(tx, id, changes);
  if (!updated) return std::unexpected(updated.error());
  return {};
}

Status ban(db::Tx& tx, std::int64_t id) {
  Arena arena(1024);
  // `create_bans_from_sessions`: `sessions.pluck(:ip_address).compact_blank.uniq`
  auto ips = tx.conn().all(kSessionIps, arena, id);
  if (!ips) return std::unexpected(ips.error());
  std::vector<std::string> seen;
  const std::string now = tx.now_db();
  for (const IpRow& row : *ips) {
    if (!row.ip || blank(*row.ip)) continue;
    if (std::find(seen.begin(), seen.end(), *row.ip) != seen.end()) continue;
    seen.emplace_back(*row.ip);
    if (const auto problem = bans::validate(*row.ip))
      return fail(Errc::InvalidArgument, "Validation failed: " + *problem);
    auto inserted = tx.conn().first(kBanInsert, arena, now, *row.ip, now, id);
    if (!inserted) return std::unexpected(inserted.error());
    tx.changed(db::schema::Table::Bans, **inserted);
  }
  // `apply_ban`
  close_remote_connections(tx, id, false);
  if (auto done = tx.conn().exec(kDeleteSessions, id); !done) return std::unexpected(done.error());
  tx.after_commit([id] { hooks::remove_banned_content(id); });
  UserChanges changes;
  changes.status = kStatusBanned;
  auto updated = update(tx, id, changes);
  if (!updated) return std::unexpected(updated.error());
  return {};
}

Status unban(db::Tx& tx, std::int64_t id) {
  if (auto done = tx.conn().exec(kDeleteBans, id); !done) return std::unexpected(done.error());
  UserChanges changes;
  changes.status = kStatusActive;
  auto updated = update(tx, id, changes);
  if (!updated) return std::unexpected(updated.error());
  return {};
}

Result<User> create_bot(db::Tx& tx, std::string_view name, const std::optional<std::string>& webhook_url) {
  NewUser attributes;
  attributes.name = std::string(name);
  attributes.role = Role::Bot;
  attributes.bot_token = generate_bot_token();
  auto with_token = create(tx, attributes);
  if (!with_token) return std::unexpected(with_token.error());
  if (webhook_url) {
    Arena arena(256);
    const std::string now = tx.now_db();
    auto inserted = tx.conn().first(kWebhookInsert, arena, now, now, view(webhook_url), with_token->id);
    if (!inserted) return std::unexpected(inserted.error());
    tx.changed(db::schema::Table::Webhooks, **inserted);
  }
  return with_token;
}

Status update_bot(db::Tx& tx, std::int64_t id, const UserChanges& changes,
                  const std::optional<std::string>& webhook_url) {
  Arena arena(256);
  auto webhook = tx.conn().first(kWebhook, arena, id);
  if (!webhook) return std::unexpected(webhook.error());
  const bool present = webhook_url && !blank(*webhook_url);
  const std::string now = tx.now_db();
  if (present && *webhook) {
    if ((*webhook)->url != std::optional<std::string_view>(*webhook_url)) {
      if (auto done = tx.conn().exec(kWebhookUpdate, now, *webhook_url, (*webhook)->id); !done) {
        return std::unexpected(done.error());
      }
    }
  } else if (present) {
    auto inserted = tx.conn().first(kWebhookInsert, arena, now, now, view(webhook_url), id);
    if (!inserted) return std::unexpected(inserted.error());
  } else if (*webhook) {
    if (auto done = tx.conn().exec(kWebhookDelete, (*webhook)->id); !done) return std::unexpected(done.error());
  }
  tx.changed(db::schema::Table::Webhooks, id);
  auto updated = update(tx, id, changes);
  if (!updated) return std::unexpected(updated.error());
  return {};
}

Status reset_bot_key(db::Tx& tx, std::int64_t id) {
  UserChanges changes;
  changes.bot_token = std::optional<std::string>(generate_bot_token());
  auto updated = update(tx, id, changes);
  if (!updated) return std::unexpected(updated.error());
  return {};
}

}  // namespace users
}  // namespace campfire::models
