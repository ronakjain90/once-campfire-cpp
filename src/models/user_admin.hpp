// The writes and lists of the user area. Rails: app/models/user.rb, user/bot.rb, user/bannable.rb, ban.rb, webhook.rb.
// Rust: crates/db/src/models/user.rs, ban.rs, webhook.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/arena.hpp"
#include "core/error.hpp"
#include "db/connection.hpp"
#include "db/database.hpp"
#include "models/user.hpp"

namespace campfire::models {

// The attributes of `user.update`. A member that is not set leaves the attribute alone.
struct UserChanges {
  std::optional<std::string> name;
  std::optional<std::optional<std::string>> email_address;
  std::optional<std::string> password_digest;
  std::optional<std::int64_t> role;
  std::optional<std::int64_t> status;
  std::optional<std::optional<std::string>> bio;
  std::optional<std::optional<std::string>> bot_token;
};

// `enum :status`
inline constexpr std::int64_t kStatusDeactivated = 1;
inline constexpr std::int64_t kStatusBanned = 2;

namespace users {

// `User.active.find(id)`
[[nodiscard]] Result<std::optional<User>> find_active(db::Connection& conn, Arena& arena, std::int64_t id);
// `User.active_bots.find(id)`
[[nodiscard]] Result<std::optional<User>> find_active_bot(db::Connection& conn, Arena& arena, std::int64_t id);
// `User.active.ordered.without_bots`, or `User.where(status: [ :active, :banned ])...` with `with_banned`.
[[nodiscard]] Result<std::vector<User>> account_users(db::Connection& conn, Arena& arena, bool with_banned);
// `User.active_bots.ordered`
[[nodiscard]] Result<std::vector<User>> active_bots_ordered(db::Connection& conn, Arena& arena);
// `bot.webhook_url`
[[nodiscard]] Result<std::optional<std::string>> webhook_url(db::Connection& conn, Arena& arena, std::int64_t user_id);
// `bot.rooms.without_directs.ordered`: the id and the name of each room.
struct BotRoom {
  std::int64_t id = 0;
  std::string name;
};
[[nodiscard]] Result<std::vector<BotRoom>> bot_rooms(db::Connection& conn, Arena& arena, std::int64_t user_id);

// `user.update(changes)`: it reads the row again on the writer, writes only when a value changes, and
// sets `updated_at` then. It gives the new row. Records a change of `users`.
[[nodiscard]] Result<User> update(db::Tx& tx, std::int64_t id, const UserChanges& changes);
// `user.deactivate`. The callback of the sockets runs after the commit.
[[nodiscard]] Status deactivate(db::Tx& tx, std::int64_t id);
// `user.ban` and `user.unban`.
[[nodiscard]] Status ban(db::Tx& tx, std::int64_t id);
[[nodiscard]] Status unban(db::Tx& tx, std::int64_t id);
// `User.create_bot!`: the bot, and its webhook if `webhook_url` is given (any value, an empty one too).
[[nodiscard]] Result<User> create_bot(db::Tx& tx, std::string_view name, const std::optional<std::string>& webhook_url);
// `bot.update_bot!`: the webhook first, then the user.
[[nodiscard]] Status update_bot(db::Tx& tx, std::int64_t id, const UserChanges& changes,
                                const std::optional<std::string>& webhook_url);
// `bot.reset_bot_key`
[[nodiscard]] Status reset_bot_key(db::Tx& tx, std::int64_t id);
// `User.generate_bot_token`: `SecureRandom.alphanumeric(12)`.
[[nodiscard]] std::string generate_bot_token();
// `user.bot_key`
[[nodiscard]] inline std::string bot_key(const User& user) {
  return std::to_string(user.id) + "-" + user.bot_token.value_or("");
}

}  // namespace users

namespace bans {

// `ip_address_is_public`: the message of the error, or nothing if the address is valid.
[[nodiscard]] std::optional<std::string> validate(std::string_view ip_address);

}  // namespace bans
}  // namespace campfire::models
