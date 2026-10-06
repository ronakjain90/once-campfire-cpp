// Users::SidebarsController and what the room controllers share with it. Rails:
// app/controllers/users/sidebars_controller.rb. Rust: crates/campfire/src/controllers/presenters/accounts.rs.
#pragma once

#include "app/flow.hpp"
#include "app/rq.hpp"
#include "models/membership.hpp"
#include "models/room.hpp"
#include "models/user.hpp"
#include "views/users/sidebar.hpp"

namespace campfire::app::controllers {

// The user as the views see it: the name, the title and the avatar path.
[[nodiscard]] views::UserSummary user_summary(const Rq& rq, const models::User& user);

// `users/sidebars/rooms/_direct` locals: `room.users.without(membership.user).presence || [ membership.user ]`.
[[nodiscard]] Flow<views::SidebarDirect> sidebar_direct(Rq& rq, const models::Membership& membership,
                                                        const models::Room& room);

}  // namespace campfire::app::controllers
