// Rooms::DirectsController. Rails: app/controllers/rooms/directs_controller.rb.
// Rust: crates/campfire/src/controllers/rooms/directs.rs.
#include <algorithm>
#include <array>

#include "app/broadcasts.hpp"
#include "app/concerns.hpp"
#include "app/controllers/rooms.hpp"
#include "app/controllers/sidebars.hpp"
#include "app/dispatch.hpp"
#include "app/page.hpp"
#include "models/membership.hpp"
#include "models/user.hpp"
#include "views/rooms/pages.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::controllers {

// `show` is RoomsController's, without `set_room`: `remember_last_room_visited` raises on the nil `@room` in Rails
// for a room that exists. The Rust port redirects to the room page.
Task<Flow<net::Response>> directs_show(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  const auto raw = rq.param_str("id");
  const auto id = raw ? cast_id(*raw) : std::nullopt;
  if (!id) co_return fail_with(ErrorKind::NotFound, "Couldn't find Room");
  co_return redirect_to_room(rq, *id);
}

Task<Flow<net::Response>> directs_new(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  db::DependencyScope& deps = rq.track();
  auto layout = load_layout(rq);
  if (!layout) co_return std::unexpected(std::move(layout.error()));
  if (auto format = ensure_html(rq); !format) co_return std::unexpected(std::move(format.error()));
  add_page_facets(rq, deps, "rooms/directs#new");
  const LayoutData& data = *layout;
  const auto render = [&](Out& out) {
    const views::ViewContext ctx = make_view_context(rq, data);
    views::LayoutParts parts;
    views::rooms::directs_new(parts, ctx);
    render_in_layout(rq, data, parts, out);
  };
  co_return cached_page(rq, 200, deps, render);
}

Task<Flow<net::Response>> directs_create(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  const std::int64_t user_id = rq.current_user()->id;
  // `User.where(id: selected_users_ids.including(Current.user.id))`
  std::vector<std::int64_t> ids = user_ids_param(rq);
  ids.push_back(user_id);
  std::optional<models::Room> room;
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Status {
    Arena arena(1024);
    auto users = models::users::existing_ids(tx.conn(), arena, ids);
    if (!users) return std::unexpected(users.error());
    auto found = models::rooms::find_or_create_direct_for(tx, *users, user_id);
    if (!found) return std::unexpected(found.error());
    room = std::move(*found);
    return {};
  });
  if (!written) co_return fail_internal(written.error().message);
  // `broadcast_create_room`: the direct partial of each membership, to its user.
  auto layout = load_layout(rq);
  if (!layout) co_return std::unexpected(std::move(layout.error()));
  const views::ViewContext ctx = make_view_context(rq, *layout);
  auto memberships = models::memberships::for_room(rq.db(), rq.arena(), room->id);
  if (!memberships) co_return fail_internal(memberships.error().message);
  for (const models::Membership& membership : *memberships) {
    auto direct = sidebar_direct(rq, membership, *room);
    if (!direct) co_return std::unexpected(std::move(direct.error()));
    Out out;
    views::users::sidebars::rooms::direct(out, ctx, *direct);
    broadcasts::direct_room_create(rq.app, membership.user_id, out.to_string());
  }
  co_return redirect_to_room(rq, room->id);
}

Task<Flow<net::Response>> directs_edit(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  db::DependencyScope& deps = rq.track();
  auto room = set_room(rq, models::RoomScope::Directs);
  if (!room) co_return std::unexpected(std::move(room.error()));
  auto layout = load_layout(rq);
  if (!layout) co_return std::unexpected(std::move(layout.error()));
  const models::User& current = *rq.current_user();
  auto members = models::users::of_room(rq.db(), rq.arena(), room->id);
  if (!members) co_return fail_internal(members.error().message);
  // `@room.users.many? ? @room.users.without(Current.user) : @room.users`
  views::DirectEditView edit;
  edit.room_id = room->id;
  std::vector<std::string> others;
  for (const models::User& member : *members) {
    if (member.id != current.id) others.push_back(member.name);
  }
  for (const models::User& member : *members) {
    if (members->size() > 1 && member.id == current.id) continue;
    edit.users.push_back(user_summary(rq, member));
  }
  // `room_display_name(@room)`: the other members' names as a sentence, or the user's own name.
  const std::string sentence = views::helpers::to_sentence(others, " and ");
  edit.display_name = sentence.find_first_not_of(" \t\n\v\f\r") == std::string::npos ? current.name : sentence;
  if (auto format = ensure_html(rq); !format) co_return std::unexpected(std::move(format.error()));
  add_page_facets(rq, deps, "rooms/directs#edit");
  const LayoutData& data = *layout;
  const auto render = [&](Out& out) {
    const views::ViewContext ctx = make_view_context(rq, data);
    views::LayoutParts parts;
    std::string title;
    views::rooms::directs_edit(parts, title, ctx, edit);
    render_in_layout(rq, data, parts, out);
  };
  co_return cached_page(rq, 200, deps, render);
}

Task<Flow<net::Response>> directs_destroy(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto room = set_room(rq, models::RoomScope::Directs);
  if (!room) co_return std::unexpected(std::move(room.error()));
  // `ensure_can_administer` always passes: every member of a direct room can.
  co_return co_await destroy_room(rq, std::move(*room));
}

}  // namespace campfire::app::controllers

namespace campfire::routes::rooms_directs {

Task<net::Response> show(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::directs_show);
}
Task<net::Response> new_(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::directs_new);
}
Task<net::Response> create(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::directs_create);
}
Task<net::Response> edit(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::directs_edit);
}
Task<net::Response> destroy(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::directs_destroy);
}

}  // namespace campfire::routes::rooms_directs
