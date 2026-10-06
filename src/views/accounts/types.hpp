// The view models of the account, user, profile, bot and push subscription pages. Rust: crates/views/src/accounts.rs,
// crates/views/src/users.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "views/rooms/forms.hpp"
#include "views/users/sidebar.hpp"

namespace campfire::views {

// A user of the people list (`accounts/users/_user`).
struct AccountUser {
  UserSummary user;
  bool banned = false;
  bool active = false;
  bool bot = false;
  bool administrator = false;
};

// `accounts/edit`.
struct AccountEditView {
  std::int64_t account_id = 0;
  std::string join_code;
  bool restrict_room_creation_to_administrators = false;
  std::vector<AccountUser> administrators;
  std::vector<AccountUser> members;
  // `@page.next_param` unless `@page.last?`
  std::optional<std::string> next_page;
};

// `accounts/users/index.turbo_stream`.
struct AccountUsersStreamView {
  std::vector<AccountUser> users;
  std::optional<std::string> next_page;
};

struct BotRoomView {
  std::int64_t id = 0;
  std::string name;
};

// A bot of `accounts/bots/_bot`.
struct BotView {
  UserSummary user;
  std::string bot_key;
  std::vector<BotRoomView> rooms;
};

// The fields of `accounts/bots/_form`.
struct BotFormView {
  std::optional<std::string> name;
  std::optional<std::string> webhook_url;
  // `bot.avatar` as `image_tag` makes its URL: the absolute path of the blob redirect.
  std::optional<std::string> avatar_attachment_url;
};

// `users/show`.
struct UserShowView {
  UserSummary user;
  std::optional<std::string> email_address;
  std::optional<std::string> bio;
  bool bot = false;
  bool active = false;
  bool deactivated = false;
  bool banned = false;
  bool is_current = false;  // `Current.user == @user`
  // `session_transfer_url(user.transfer_id)`
  std::string transfer_url;
};

// A membership of `users/profiles/_membership`.
struct ProfileMembership {
  std::int64_t room_id = 0;
  RoomKind kind = RoomKind::Open;
  std::string room_display_name;
  std::string involvement;
  [[nodiscard]] InvolvementView involvement_view() const { return {room_id, kind, involvement}; }
};

// `users/profiles/show`.
struct ProfileShowView {
  UserSummary user;
  std::optional<std::string> email_address;
  std::optional<std::string> bio;
  bool avatar_attached = false;
  std::string transfer_url;
  std::vector<ProfileMembership> shared_memberships;
  std::vector<ProfileMembership> direct_memberships;
};

// A subscription of `users/push_subscriptions/_push_subscription`.
struct PushSubscriptionView {
  std::int64_t id = 0;
  std::string endpoint;
  std::string browser;
  std::string version;
  std::string platform;
};

}  // namespace campfire::views
