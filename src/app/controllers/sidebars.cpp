// Users::SidebarsController. Rails: app/controllers/users/sidebars_controller.rb.
// Rust: crates/campfire/src/controllers/users/sidebars.rs, presenters/accounts.rs (sidebar).
#include <algorithm>
#include <array>
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

namespace {

views::UserSummary summary(const Rq& rq, const models::User& user) {
  views::UserSummary out;
  out.id = user.id;
  out.name = user.name;
  out.avatar_path = user_avatar_path(rq, user);
  return out;
}

// `room.updated_at.to_fs(:epoch)`.
std::string epoch_of(const std::string& db_text) {
  const auto t = parse_db(db_text);
  return t ? std::to_string(epoch_ms(*t)) : std::string{};
}

// `users/sidebars/rooms/_direct` locals: `room.users.without(membership.user).presence || [ membership.user ]`.
Flow<views::SidebarDirect> sidebar_direct(Rq& rq, const models::MembershipWithRoom& item) {
  auto users = models::users::of_room(rq.db(), rq.arena(), item.room.id);
  if (!users) return fail_internal(users.error().message);
  views::SidebarDirect direct;
  direct.room_id = item.room.id;
  direct.unread = item.membership.unread();
  direct.updated_at_epoch = epoch_of(item.room.updated_at);
  for (const models::User& user : *users) {
    if (user.id != item.membership.user_id) direct.members.push_back(summary(rq, user));
  }
  if (direct.members.empty()) {
    auto own = models::users::find_by_id(rq.db(), rq.arena(), item.membership.user_id);
    if (!own) return fail_internal(own.error().message);
    if (*own) direct.members.push_back(summary(rq, **own));
  }
  return direct;
}

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
  for (const models::MembershipWithRoom* item : directs) {
    auto direct = sidebar_direct(rq, *item);
    if (!direct) co_return std::unexpected(std::move(direct.error()));
    sidebar.direct_memberships.push_back(std::move(*direct));
  }
  auto placeholders = models::users::direct_placeholders(rq.db(), rq.arena(), user.id);
  if (!placeholders) co_return fail_internal(placeholders.error().message);
  for (const models::User& placeholder : *placeholders) {
    sidebar.direct_placeholder_users.push_back(summary(rq, placeholder));
  }
  auto account = models::accounts::first(rq.db(), rq.arena());
  if (!account) co_return fail_internal(account.error().message);
  const bool restricted = *account && (*account)->restrict_room_creation_to_administrators;
  sidebar.can_create_rooms = user.is_administrator() || !restricted;
  sidebar.current_user = summary(rq, user);
  // `turbo_stream_from :rooms`, `turbo_stream_from Current.user, :rooms`
  const std::array<std::string_view, 1> rooms{"rooms"};
  sidebar.rooms_stream = compat::turbo::signed_stream_name(rq.app.secrets, rooms);
  const std::string user_gid = compat::global_id::GlobalId::make("User", std::to_string(user.id)).to_param();
  const std::array<std::string_view, 2> user_rooms{user_gid, "rooms"};
  sidebar.user_rooms_stream = compat::turbo::signed_stream_name(rq.app.secrets, user_rooms);

  // Everything that is not SQL and that the page prints.
  deps.facet("page", "users/sidebars#show");
  deps.facet("base_url", rq.info.base_url());
  deps.facet("frame", static_cast<std::uint64_t>(rq.is_turbo_frame_request()));
  deps.facet("user", static_cast<std::uint64_t>(user.id));
  deps.facet("user_updated_at", user.updated_at);
  const auto notice = rq.flash().notice();
  const auto alert = rq.flash().alert();
  deps.facet("notice", notice.value_or(""));
  deps.facet("alert", alert.value_or(""));
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
