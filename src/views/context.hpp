// Per-request state that the layouts and helpers read (Rust: crates/views/src/lib.rs ViewContext).
// Plain data. A later task fills it from the request, the session and the account.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace campfire::views {

struct CurrentUser {
  std::int64_t id = 0;
  std::string name;
  bool administrator = false;
  bool bot = false;
  // `fresh_user_avatar_path(Current.user)`.
  std::string avatar_url;
};

struct AccountSummary {
  std::string name;
  // `fresh_account_logo_path` (no size).
  std::string logo_url;
  // `Current.account.logo.attached?`: adds the `account-has-logo` body class.
  bool has_logo = false;
};

// `ApplicationPlatform` as the views see it (Rust: campfire_views::Platform). A predicate that raises in Ruby is false.
struct Platform {
  bool ios = false;
  bool android = false;
  bool mac = false;
  bool windows = false;
  bool chrome = false;
  bool firefox = false;
  bool safari = false;
  bool edge = false;
  bool mobile = false;
  bool desktop = false;
  bool apple_messages = false;
  std::string browser;
  std::string operating_system;
};

struct ViewContext {
  std::optional<CurrentUser> current_user;
  AccountSummary account;
  std::optional<std::string> flash_notice;
  std::optional<std::string> flash_alert;
  // `Rails.configuration.x.vapid.public_key`; nullopt leaves out the content attribute.
  std::optional<std::string> vapid_public_key;
  // Resolves a logical asset path ("campfire-icon.png") to its digested URL.
  std::function<std::string(std::string_view)> asset_path;
  // `javascript_importmap_tags` and `stylesheet_link_tag :all`: the HTML that the asset task builds.
  std::string importmap_tags;
  std::string stylesheet_tags;
  // The custom CSS of the account, if any.
  std::optional<std::string> custom_styles;
  // `script_aware_action_cable_meta_tag` content: script_name + "/cable".
  std::string cable_url = "/cable";
  // `request.base_url`, `request.url` and `request.referrer`.
  std::string base_url;
  std::string request_url;
  std::optional<std::string> referrer;
  // The id of `last_room_visited`; nullopt links back to the root.
  std::optional<std::int64_t> last_room_visited_id;
  std::string app_version = "0";
  // `platform`: the User-Agent of the request.
  Platform platform;

  [[nodiscard]] std::string asset(std::string_view logical_path) const { return asset_path(logical_path); }
  // `root_url`, `join_url(...)`: the base URL and the path.
  [[nodiscard]] std::string url(std::string_view path) const { return base_url + std::string(path); }
  [[nodiscard]] bool can_administer() const { return current_user && current_user->administrator; }
};

}  // namespace campfire::views
