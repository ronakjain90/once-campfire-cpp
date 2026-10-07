// Rails: app/models/push/subscription.rb. Rust: crates/db/src/models/push_subscription.rs.
// This file has the rows, the validation and the queries of `Room::MessagePusher`. The delivery is in
// src/app/web_push.cpp.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/arena.hpp"
#include "core/error.hpp"
#include "db/connection.hpp"
#include "db/database.hpp"

namespace campfire::models {

struct PushSubscription {
  std::int64_t id = 0;
  std::int64_t user_id = 0;
  std::optional<std::string> endpoint;
  std::optional<std::string> p256dh_key;
  std::optional<std::string> auth_key;
  std::optional<std::string> user_agent;
  std::string created_at;
  std::string updated_at;
};

// `RestrictedHTTP::PrivateNetworkGuard.resolve(host)`: a public address, or nothing.
using ResolveHost = std::function<std::optional<std::string>(std::string_view host)>;

namespace push_subscriptions {

// `Push::Subscription::PERMITTED_ENDPOINT_HOSTS`: the host of a valid https endpoint on port 443 that the
// subscription may call, or nothing. The caller resolves that host (the check `resolved_endpoint_ip`).
[[nodiscard]] std::optional<std::string> endpoint_host_to_resolve(const std::optional<std::string>& endpoint);
// The messages of `validate_endpoint_url` and the presence check (`full_messages`), with `resolve` for the DNS check.
[[nodiscard]] std::vector<std::string> validate(const PushSubscription& subscription, const ResolveHost& resolve);

// `user.push_subscriptions`
[[nodiscard]] Result<std::vector<PushSubscription>> for_user(db::Connection& conn, Arena& arena, std::int64_t user_id);
// `Push::Subscription.find(id)` where the user owns it.
[[nodiscard]] Result<std::optional<PushSubscription>> find_for_user(db::Connection& conn, Arena& arena,
                                                                    std::int64_t user_id, std::int64_t id);
// `user.push_subscriptions.find_by(params)`: only the given keys are conditions; a nil value is `IS NULL`.
struct Conditions {
  bool endpoint_given = false;
  std::optional<std::string> endpoint;
  bool p256dh_key_given = false;
  std::optional<std::string> p256dh_key;
  bool auth_key_given = false;
  std::optional<std::string> auth_key;
};
[[nodiscard]] Result<std::optional<PushSubscription>> find_by(db::Connection& conn, Arena& arena, std::int64_t user_id,
                                                              const Conditions& conditions);
// `subscription.touch`
[[nodiscard]] Status touch(db::Tx& tx, std::int64_t id);
// `create`: validates, then inserts. A failed validation gives `InvalidArgument` with the messages.
[[nodiscard]] Result<PushSubscription> create(db::Tx& tx, const PushSubscription& subscription,
                                              const ResolveHost& resolve);
// `destroy_by(id:)` for the user.
[[nodiscard]] Status destroy_by_id(db::Tx& tx, std::int64_t user_id, std::int64_t id);
// `Push::Subscription.find_by(id:)`
[[nodiscard]] Result<std::optional<PushSubscription>> find(db::Connection& conn, Arena& arena, std::int64_t id);
// `subscription.destroy`
[[nodiscard]] Status destroy(db::Tx& tx, std::int64_t id);
// `Room::MessagePusher#push_subscriptions_for_users_involved_in_everything`: the subscriptions of the members of the
// room who are not the creator, are not connected (`connected_at` before `cutoff` or nothing) and want everything.
// `cutoff` is `CONNECTION_TTL.ago`, as the text of the database.
[[nodiscard]] Result<std::vector<PushSubscription>> involved_in_everything(db::Connection& conn, Arena& arena,
                                                                           std::int64_t room_id,
                                                                           std::int64_t creator_id,
                                                                           std::string_view cutoff);
// `push_subscriptions_for_mentionable_users(mentionees)`: the same for the members who want mentions and are in
// `user_ids`.
[[nodiscard]] Result<std::vector<PushSubscription>> involved_in_mentions(db::Connection& conn, Arena& arena,
                                                                         std::int64_t room_id, std::int64_t creator_id,
                                                                         const std::vector<std::int64_t>& user_ids,
                                                                         std::string_view cutoff);
// `user.memberships.unread.count`: the badge of a notification.
[[nodiscard]] Result<std::int64_t> unread_count(db::Connection& conn, Arena& arena, std::int64_t user_id);

}  // namespace push_subscriptions
}  // namespace campfire::models
