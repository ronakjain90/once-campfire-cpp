// The view models of the sidebar. Rust: crates/views/src/users.rs (SidebarDirect, SidebarRoom, SidebarShow).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "views/helpers/users.hpp"

namespace campfire::views {

// A user as the room views see it. Rust: crates/views/src/users/summary.rs (UserSummary).
struct UserSummary {
  std::int64_t id = 0;
  std::string name;
  // `User#title`: the name and the bio joined by " – ".
  std::string title;
  // `fresh_user_avatar_path(user)`.
  std::string avatar_path;

  // `name.split(' ')`: awk-style, on ASCII white space.
  [[nodiscard]] std::vector<std::string_view> name_parts() const {
    std::vector<std::string_view> parts;
    std::size_t i = 0;
    while (i < name.size()) {
      while (i < name.size() && (name[i] == ' ' || (name[i] >= '\t' && name[i] <= '\r'))) ++i;
      const std::size_t start = i;
      while (i < name.size() && !(name[i] == ' ' || (name[i] >= '\t' && name[i] <= '\r'))) ++i;
      if (i > start) parts.emplace_back(name.data() + start, i - start);
    }
    return parts;
  }
  // `name.split(' ')[0]`.
  [[nodiscard]] std::string_view first_name() const {
    const auto parts = name_parts();
    return parts.empty() ? std::string_view{} : parts.front();
  }
};

// A direct room in the sidebar (`users/sidebars/rooms/_direct`).
struct SidebarDirect {
  std::int64_t room_id = 0;
  bool unread = false;
  // `room.updated_at.to_fs(:epoch)`.
  std::string updated_at_epoch;
  // `room.users.without(membership.user).presence || [ membership.user ]`.
  std::vector<UserSummary> members;

  // `members.first(4)`.
  [[nodiscard]] std::span<const UserSummary> first_members() const {
    return std::span<const UserSummary>(members).first(members.size() < 4 ? members.size() : 4);
  }
  // `[ "direct", "unread": membership.unread? ]`.
  [[nodiscard]] std::string_view class_names() const { return unread ? "direct unread" : "direct"; }
  // `members.map { |m| m.name.split(' ')[0, 3].map { |s| s[0].capitalize }.join }.to_sentence(two_words_connector:
  // '+')`.
  [[nodiscard]] std::string member_initials() const {
    std::vector<std::string> initials;
    for (const UserSummary& member : members) {
      std::string one;
      const auto parts = member.name_parts();
      for (std::size_t i = 0; i < parts.size() && i < 3; ++i) {
        one += helpers::capitalize(helpers::first_character(parts[i]));
      }
      initials.push_back(std::move(one));
    }
    return helpers::to_sentence(initials, "+");
  }
};

// A shared room in the sidebar (`users/sidebars/rooms/_shared`).
struct SidebarRoom {
  std::int64_t id = 0;
  // `model_name.param_key`: "rooms_open" or "rooms_closed".
  std::string param_key;
  std::string name;
  bool unread = false;

  [[nodiscard]] std::string_view class_names() const {
    return unread ? "align-center gap room btn txt-nowrap unread" : "align-center gap room btn txt-nowrap";
  }
};

// The data of `users/sidebars/show`.
struct SidebarShow {
  UserSummary current_user;
  // `Turbo::StreamsChannel.signed_stream_name(:rooms)` and `([ Current.user, :rooms ])`.
  std::string rooms_stream;
  std::string user_rooms_stream;
  std::vector<SidebarDirect> direct_memberships;
  std::vector<UserSummary> direct_placeholder_users;
  std::vector<SidebarRoom> other_memberships;
  // `Current.user.administrator? || !Current.account.settings.restrict_room_creation_to_administrators?`.
  bool can_create_rooms = false;
};

}  // namespace campfire::views
