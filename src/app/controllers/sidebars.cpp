// Users::SidebarsController. Rails: app/controllers/users/sidebars_controller.rb.
// Rust: crates/campfire/src/controllers/users/sidebars.rs, presenters/accounts.rs (sidebar).
#include "app/controllers/sidebars.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <string>

#include "app/concerns.hpp"
#include "app/dispatch.hpp"
#include "app/page.hpp"
#include "compat/global_id.hpp"
#include "compat/turbo.hpp"
#include "core/time_format.hpp"
#include "models/account.hpp"
#include "models/membership.hpp"
#include "models/user.hpp"
#include "views/helpers/application.hpp"
#include "views/templates.gen.hpp"
#include "views/users/sidebar.hpp"

namespace campfire::app::controllers {

views::UserSummary user_summary(const Rq& rq, const models::User& user) {
  views::UserSummary out;
  out.id = user.id;
  out.name = user.name;
  out.title = views::helpers::user_title(user.name, user.bio);
  out.avatar_path = user_avatar_path(rq, user);
  return out;
}

namespace {

// The `users/sidebars/rooms/_direct` locals, from the members of the room in the order of the query.
Flow<views::SidebarDirect> direct_locals(Rq& rq, const models::Membership& membership, const models::Room& room,
                                         std::span<const models::User* const> members) {
  views::SidebarDirect direct;
  direct.room_id = room.id;
  direct.unread = membership.unread();
  const auto updated = parse_db(room.updated_at);
  if (updated) direct.updated_at_epoch = std::to_string(epoch_ms(*updated));
  for (const models::User* user : members) {
    if (user->id != membership.user_id) direct.members.push_back(user_summary(rq, *user));
  }
  if (direct.members.empty()) {
    auto own = models::users::find_by_id(rq.db(), rq.arena(), membership.user_id);
    if (!own) return fail_internal(own.error().message);
    if (*own) direct.members.push_back(user_summary(rq, **own));
  }
  return direct;
}

}  // namespace

Flow<views::SidebarDirect> sidebar_direct(Rq& rq, const models::Membership& membership, const models::Room& room) {
  auto users = models::users::of_room(rq.db(), rq.arena(), room.id);
  if (!users) return fail_internal(users.error().message);
  std::vector<const models::User*> members;
  for (const models::User& user : *users) members.push_back(&user);
  return direct_locals(rq, membership, room, members);
}

namespace {

Task<Flow<net::Response>> sidebars_show(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  const req::Format offered[] = {&req::mime::HTML};
  if (auto format = rq.respond_to(offered); !format) co_return std::unexpected(std::move(format.error()));
  const models::User& user = *rq.current_user();
  db::DependencyScope& deps = rq.track();
  auto layout = load_layout(rq);
  if (!layout) co_return std::unexpected(std::move(layout.error()));

  // `@direct_memberships, @other_memberships = memberships.visible.with_ordered_room.partition { direct? }`
  auto all = models::memberships::visible_with_ordered_room(rq.db(), rq.arena(), user.id);
  if (!all) co_return fail_internal(all.error().message);
  std::vector<const models::MembershipWithRoom*> directs;
  views::SidebarShow sidebar;
  for (const models::MembershipWithRoom& item : *all) {
    if (item.room.is_direct()) {
      directs.push_back(&item);
    } else {
      views::SidebarRoom room;
      room.id = item.room.id;
      room.param_key = item.room.is_open() ? "rooms_open" : "rooms_closed";
      room.name = item.room.name.value_or("");
      room.unread = item.membership.unread();
      sidebar.other_memberships.push_back(std::move(room));
    }
  }
  // `sort_by { |membership| membership.room.updated_at }.reverse`: stable in Ruby's sort is not promised, the
  // reverse of an ascending order keeps equal keys in reverse order.
  std::stable_sort(directs.begin(), directs.end(),
                   [](const auto* a, const auto* b) { return a->room.updated_at < b->room.updated_at; });
  std::reverse(directs.begin(), directs.end());
  // One statement reads the members of all direct rooms. The rows come by room id, so a room is a run of rows.
  auto members = models::users::direct_room_members(rq.db(), rq.arena(), user.id);
  if (!members) co_return fail_internal(members.error().message);
  for (const models::MembershipWithRoom* item : directs) {
    std::vector<const models::User*> room_members;
    const auto first = std::lower_bound(members->begin(), members->end(), item->room.id,
                                        [](const models::users::RoomMember& m, std::int64_t id) { return m.room_id < id; });
    for (auto it = first; it != members->end() && it->room_id == item->room.id; ++it) room_members.push_back(&it->user);
    auto direct = direct_locals(rq, item->membership, item->room, room_members);
    if (!direct) co_return std::unexpected(std::move(direct.error()));
    sidebar.direct_memberships.push_back(std::move(*direct));
  }
  auto placeholders = models::users::direct_placeholders(rq.db(), rq.arena(), user.id, *members);
  if (!placeholders) co_return fail_internal(placeholders.error().message);
  for (const models::User& placeholder : *placeholders) {
    sidebar.direct_placeholder_users.push_back(user_summary(rq, placeholder));
  }
  // `Current.account` is the row that `load_layout` read.
  const bool restricted = layout->restrict_room_creation_to_administrators;
  sidebar.can_create_rooms = user.is_administrator() || !restricted;
  sidebar.current_user = user_summary(rq, user);
  // `turbo_stream_from :rooms`, `turbo_stream_from Current.user, :rooms`: the names have no expiry, so the worker
  // keeps them.
  sidebar.rooms_stream = rq.worker.memo("stream:rooms", [&] {
    const std::array<std::string_view, 1> rooms{"rooms"};
    return compat::turbo::signed_stream_name(rq.app.secrets, rooms);
  });
  sidebar.user_rooms_stream = rq.worker.memo("stream:user:" + std::to_string(user.id) + ":rooms", [&] {
    const std::string user_gid = compat::global_id::GlobalId::make("User", std::to_string(user.id)).to_param();
    const std::array<std::string_view, 2> user_rooms{user_gid, "rooms"};
    return compat::turbo::signed_stream_name(rq.app.secrets, user_rooms);
  });

  add_page_facets(rq, deps, "users/sidebars#show");
  const LayoutData& data = *layout;
  const auto render = [&](Out& out) {
    views::LayoutParts parts;
    const views::ViewContext ctx = make_view_context(rq, data);
    parts.content = [&](Out& o) { views::users::sidebars::show(o, ctx, sidebar); };
    render_in_layout(rq, data, parts, out);
  };
  co_return cached_page(rq, 200, deps, render);
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::users_sidebars {

Task<net::Response> show(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::sidebars_show);
}

}  // namespace campfire::routes::users_sidebars
