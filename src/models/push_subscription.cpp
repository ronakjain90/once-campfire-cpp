// Rails: app/models/push/subscription.rb. Rust: crates/db/src/models/push_subscription.rs.
#include "models/push_subscription.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace campfire::models::push_subscriptions {

namespace {

#define CF_PS_COLUMNS                                                                                           \
  "\"push_subscriptions\".\"id\", \"push_subscriptions\".\"auth_key\", \"push_subscriptions\".\"created_at\", " \
  "\"push_subscriptions\".\"endpoint\", \"push_subscriptions\".\"p256dh_key\", "                                \
  "\"push_subscriptions\".\"updated_at\", \"push_subscriptions\".\"user_agent\", \"push_subscriptions\".\"user_id\""

using Row = db::schema::PushSubscriptionRow;

PushSubscription convert(const Row& row) {
  PushSubscription s;
  s.id = row.id;
  s.user_id = row.user_id;
  if (row.endpoint) s.endpoint = std::string(*row.endpoint);
  if (row.p256dh_key) s.p256dh_key = std::string(*row.p256dh_key);
  if (row.auth_key) s.auth_key = std::string(*row.auth_key);
  if (row.user_agent) s.user_agent = std::string(*row.user_agent);
  s.created_at = std::string(row.created_at);
  s.updated_at = std::string(row.updated_at);
  return s;
}

const db::Query<Row(std::int64_t)> kForUser{
    "SELECT " CF_PS_COLUMNS " FROM \"push_subscriptions\" WHERE \"push_subscriptions\".\"user_id\" = ?"};
const db::Query<Row(std::int64_t, std::int64_t)> kFind{
    "SELECT " CF_PS_COLUMNS
    " FROM \"push_subscriptions\" WHERE \"push_subscriptions\".\"user_id\" = ? AND \"push_subscriptions\".\"id\" = ? "
    "LIMIT 1"};
const db::Query<Row(std::int64_t, std::int64_t, std::optional<std::string_view>, std::int64_t,
                    std::optional<std::string_view>, std::int64_t, std::optional<std::string_view>)>
    kFindBy{"SELECT " CF_PS_COLUMNS
            " FROM \"push_subscriptions\" WHERE \"push_subscriptions\".\"user_id\" = ? AND (? = 0 OR "
            "\"push_subscriptions\".\"endpoint\" IS ?) AND (? = 0 OR \"push_subscriptions\".\"p256dh_key\" IS ?) AND "
            "(? = 0 OR \"push_subscriptions\".\"auth_key\" IS ?) LIMIT 1"};
const db::Query<void(std::string_view, std::int64_t)> kTouch{
    "UPDATE \"push_subscriptions\" SET \"updated_at\" = ? WHERE \"push_subscriptions\".\"id\" = ?"};
const db::Query<std::int64_t(std::optional<std::string_view>, std::string_view, std::optional<std::string_view>,
                             std::optional<std::string_view>, std::string_view, std::optional<std::string_view>,
                             std::int64_t)>
    kInsert{
        "INSERT INTO \"push_subscriptions\" (\"auth_key\", \"created_at\", \"endpoint\", \"p256dh_key\", "
        "\"updated_at\", \"user_agent\", \"user_id\") VALUES (?, ?, ?, ?, ?, ?, ?) RETURNING \"id\""};
const db::Query<void(std::int64_t, std::int64_t)> kDestroyBy{
    "DELETE FROM \"push_subscriptions\" WHERE \"push_subscriptions\".\"user_id\" = ? AND "
    "\"push_subscriptions\".\"id\" = ?"};
const db::Query<std::int64_t(std::int64_t)> kUnread{
    "SELECT COUNT(*) FROM \"memberships\" WHERE \"memberships\".\"user_id\" = ? AND \"memberships\".\"unread_at\" IS "
    "NOT "
    "NULL"};

const db::Query<Row(std::int64_t)> kFindById{
    "SELECT " CF_PS_COLUMNS " FROM \"push_subscriptions\" WHERE \"push_subscriptions\".\"id\" = ? LIMIT 1"};
const db::Query<void(std::int64_t)> kDestroy{
    "DELETE FROM \"push_subscriptions\" WHERE \"push_subscriptions\".\"id\" = ?"};
// `Membership.visible.disconnected.where(room:).where.not(user: creator)` merged with `involved_in_everything`.
const db::Query<Row(std::string_view, std::int64_t, std::int64_t)> kEverything{
    "SELECT " CF_PS_COLUMNS
    " FROM \"push_subscriptions\" INNER JOIN \"users\" ON \"users\".\"id\" = \"push_subscriptions\".\"user_id\" "
    "INNER JOIN \"memberships\" ON \"memberships\".\"user_id\" = \"users\".\"id\" WHERE "
    "(\"memberships\".\"connected_at\" IS NULL OR \"memberships\".\"connected_at\" < ?) AND "
    "\"memberships\".\"room_id\" = ? AND \"memberships\".\"user_id\" != ? AND \"memberships\".\"involvement\" = "
    "'everything'"};
const db::Query<Row(std::string_view, std::int64_t, std::int64_t, std::int64_t)> kMentions{
    "SELECT " CF_PS_COLUMNS
    " FROM \"push_subscriptions\" INNER JOIN \"users\" ON \"users\".\"id\" = \"push_subscriptions\".\"user_id\" "
    "INNER JOIN \"memberships\" ON \"memberships\".\"user_id\" = \"users\".\"id\" WHERE "
    "(\"memberships\".\"connected_at\" IS NULL OR \"memberships\".\"connected_at\" < ?) AND "
    "\"memberships\".\"room_id\" = ? AND \"memberships\".\"user_id\" != ? AND \"memberships\".\"involvement\" = "
    "'mentions' AND \"push_subscriptions\".\"user_id\" = ?"};

constexpr std::array<std::string_view, 5> kPermittedHosts = {"jmt17.google.com", "fcm.googleapis.com",
                                                             "updates.push.services.mozilla.com", "web.push.apple.com",
                                                             "notify.windows.com"};

std::optional<std::string_view> view(const std::optional<std::string>& s) {
  return s ? std::optional<std::string_view>(*s) : std::nullopt;
}

std::string lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// The parts of `URI.parse(endpoint)` that the validation reads.
struct EndpointUri {
  std::string scheme;
  std::string host;
  std::optional<int> port;
};

std::optional<EndpointUri> parse_endpoint(std::string_view endpoint) {
  if (endpoint.empty() ||
      std::any_of(endpoint.begin(), endpoint.end(), [](unsigned char c) { return std::isspace(c) != 0; })) {
    return std::nullopt;
  }
  const std::size_t sep = endpoint.find("://");
  if (sep == std::string_view::npos) return std::nullopt;
  EndpointUri uri;
  uri.scheme = lower(endpoint.substr(0, sep));
  std::string_view rest = endpoint.substr(sep + 3);
  std::string_view authority = rest.substr(0, rest.find_first_of("/?#"));
  if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos) authority = authority.substr(at + 1);
  std::optional<int> port;
  if (!authority.empty() && authority.back() != ']') {
    if (const std::size_t colon = authority.rfind(':'); colon != std::string_view::npos) {
      const std::string_view digits = authority.substr(colon + 1);
      if (digits.empty() || digits.size() > 5 ||
          !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
      }
      const int value = std::stoi(std::string(digits));
      if (value > 65535) return std::nullopt;
      port = value;
      authority = authority.substr(0, colon);
    }
  }
  uri.host = std::string(authority);
  if (!port)
    port = uri.scheme == "https"  ? std::optional<int>(443)
           : uri.scheme == "http" ? std::optional<int>(80)
                                  : std::nullopt;
  uri.port = port;
  return uri;
}

bool permitted_host(std::string_view host) {
  const std::string h = lower(host);
  if (h.empty()) return false;
  return std::any_of(kPermittedHosts.begin(), kPermittedHosts.end(), [&](std::string_view p) {
    return h == p || (h.size() > p.size() && h.ends_with(p) && h[h.size() - p.size() - 1] == '.');
  });
}

}  // namespace

std::optional<std::string> endpoint_host_to_resolve(const std::optional<std::string>& endpoint) {
  if (!endpoint) return std::nullopt;
  const auto uri = parse_endpoint(*endpoint);
  if (uri && uri->scheme == "https" && uri->port == 443 && permitted_host(uri->host)) return uri->host;
  return std::nullopt;
}

std::vector<std::string> validate(const PushSubscription& subscription, const ResolveHost& resolve) {
  std::vector<std::string> errors;
  const std::string endpoint = subscription.endpoint.value_or("");
  if (endpoint.find_first_not_of(" \t\n\v\f\r") == std::string::npos) errors.emplace_back("Endpoint can't be blank");
  const auto uri = parse_endpoint(endpoint);
  if (!uri) {
    errors.emplace_back("Endpoint is not a valid URL");
  } else if (uri->scheme != "https") {
    errors.emplace_back("Endpoint must use HTTPS");
  } else if (uri->port != 443) {
    errors.emplace_back("Endpoint must use the default HTTPS port");
  } else if (!permitted_host(uri->host)) {
    errors.emplace_back("Endpoint is not a permitted push service");
  } else if (!resolve(uri->host)) {
    errors.emplace_back("Endpoint resolves to a private or invalid IP address");
  }
  return errors;
}

Result<std::vector<PushSubscription>> for_user(db::Connection& conn, Arena& arena, std::int64_t user_id) {
  auto rows = conn.all(kForUser, arena, user_id);
  if (!rows) return std::unexpected(rows.error());
  std::vector<PushSubscription> out;
  for (const Row& row : *rows) out.push_back(convert(row));
  return out;
}

Result<std::optional<PushSubscription>> find_for_user(db::Connection& conn, Arena& arena, std::int64_t user_id,
                                                      std::int64_t id) {
  auto row = conn.first(kFind, arena, user_id, id);
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<PushSubscription>{};
  return std::optional<PushSubscription>(convert(**row));
}

Result<std::optional<PushSubscription>> find_by(db::Connection& conn, Arena& arena, std::int64_t user_id,
                                                const Conditions& c) {
  auto row = conn.first(kFindBy, arena, user_id, c.endpoint_given ? 1 : 0, view(c.endpoint), c.p256dh_key_given ? 1 : 0,
                        view(c.p256dh_key), c.auth_key_given ? 1 : 0, view(c.auth_key));
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<PushSubscription>{};
  return std::optional<PushSubscription>(convert(**row));
}

Result<std::optional<PushSubscription>> find(db::Connection& conn, Arena& arena, std::int64_t id) {
  auto row = conn.first(kFindById, arena, id);
  if (!row) return std::unexpected(row.error());
  if (!*row) return std::optional<PushSubscription>{};
  return std::optional<PushSubscription>(convert(**row));
}

Status destroy(db::Tx& tx, std::int64_t id) {
  if (auto done = tx.conn().exec(kDestroy, id); !done) return std::unexpected(done.error());
  tx.changed(db::schema::Table::PushSubscriptions, id);
  return {};
}

Result<std::vector<PushSubscription>> involved_in_everything(db::Connection& conn, Arena& arena, std::int64_t room_id,
                                                             std::int64_t creator_id, std::string_view cutoff) {
  auto rows = conn.all(kEverything, arena, cutoff, room_id, creator_id);
  if (!rows) return std::unexpected(rows.error());
  std::vector<PushSubscription> out;
  for (const Row& row : *rows) out.push_back(convert(row));
  return out;
}

Result<std::vector<PushSubscription>> involved_in_mentions(db::Connection& conn, Arena& arena, std::int64_t room_id,
                                                           std::int64_t creator_id,
                                                           const std::vector<std::int64_t>& user_ids,
                                                           std::string_view cutoff) {
  std::vector<PushSubscription> out;
  for (const std::int64_t user_id : user_ids) {
    auto rows = conn.all(kMentions, arena, cutoff, room_id, creator_id, user_id);
    if (!rows) return std::unexpected(rows.error());
    for (const Row& row : *rows) out.push_back(convert(row));
  }
  return out;
}

Status touch(db::Tx& tx, std::int64_t id) {
  if (auto done = tx.conn().exec(kTouch, tx.now_db(), id); !done) return std::unexpected(done.error());
  tx.changed(db::schema::Table::PushSubscriptions, id);
  return {};
}

Result<PushSubscription> create(db::Tx& tx, const PushSubscription& subscription, const ResolveHost& resolve) {
  if (const auto errors = validate(subscription, resolve); !errors.empty()) {
    std::string joined;
    for (const std::string& e : errors) joined += (joined.empty() ? "" : ", ") + e;
    return fail(Errc::InvalidArgument, "Validation failed: " + joined);
  }
  const std::string now = tx.now_db();
  Arena arena(256);
  auto id = tx.conn().first(kInsert, arena, view(subscription.auth_key), now, view(subscription.endpoint),
                            view(subscription.p256dh_key), now, view(subscription.user_agent), subscription.user_id);
  if (!id) return std::unexpected(id.error());
  PushSubscription saved = subscription;
  saved.id = **id;
  saved.created_at = saved.updated_at = now;
  tx.changed(db::schema::Table::PushSubscriptions, saved.id);
  return saved;
}

Status destroy_by_id(db::Tx& tx, std::int64_t user_id, std::int64_t id) {
  if (auto done = tx.conn().exec(kDestroyBy, user_id, id); !done) return std::unexpected(done.error());
  tx.changed(db::schema::Table::PushSubscriptions, id);
  return {};
}

Result<std::int64_t> unread_count(db::Connection& conn, Arena& arena, std::int64_t user_id) {
  auto row = conn.first(kUnread, arena, user_id);
  if (!row) return std::unexpected(row.error());
  return row->value_or(0);
}

}  // namespace campfire::models::push_subscriptions
