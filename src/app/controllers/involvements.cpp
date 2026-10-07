// Rooms::InvolvementsController and the RoomScoped concern. Rails: app/controllers/rooms/involvements_controller.rb,
// app/controllers/concerns/room_scoped.rb. Rust: crates/campfire/src/controllers/rooms/involvements.rs.
#include "app/broadcasts.hpp"
#include "app/concerns.hpp"
#include "app/controllers/rooms.hpp"
#include "app/dispatch.hpp"
#include "app/page.hpp"
#include "models/membership.hpp"
#include "routes/routes.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::controllers {

namespace {

struct Scoped {
  models::Membership membership;
  models::Room room;
};

// `RoomScoped#set_room`: `Current.user.memberships.find_by!(room_id: params[:room_id])`, 404 otherwise.
Flow<Scoped> set_scoped_room(Rq& rq) {
  const auto raw = rq.param_str("room_id");
  const auto room_id = raw ? cast_id(*raw) : std::nullopt;
  if (!room_id) return fail_with(ErrorKind::NotFound, "Couldn't find Membership");
  auto membership = models::memberships::find_for_user_and_room(rq.db(), rq.arena(), rq.current_user()->id, *room_id);
  if (!membership) return fail_internal(membership.error().message);
  if (!*membership) return fail_with(ErrorKind::NotFound, "Couldn't find Membership");
  auto room = models::rooms::find_by_id(rq.db(), rq.arena(), (*membership)->room_id);
  if (!room) return fail_internal(room.error().message);
  // The membership has no foreign key to the room: a missing room is a failure of the action.
  if (!*room) return fail_internal("the membership's room is gone");
  return Scoped{std::move(**membership), std::move(**room)};
}

views::RoomKind kind_of(const models::Room& room) {
  if (room.is_open()) return views::RoomKind::Open;
  if (room.is_closed()) return views::RoomKind::Closed;
  return views::RoomKind::Direct;
}

}  // namespace

Task<Flow<net::Response>> involvements_show(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  db::DependencyScope& deps = rq.track();
  auto scoped = set_scoped_room(rq);
  if (!scoped) co_return std::unexpected(std::move(scoped.error()));
  if (auto format = ensure_html(rq); !format) co_return std::unexpected(std::move(format.error()));
  auto layout = load_layout(rq);
  if (!layout) co_return std::unexpected(std::move(layout.error()));
  views::InvolvementView involvement;
  involvement.room_id = scoped->room.id;
  involvement.kind = kind_of(scoped->room);
  involvement.involvement = scoped->membership.involvement.value_or("");
  add_page_facets(rq, deps, "rooms/involvements#show");
  deps.facet("involvement", involvement.involvement);
  const LayoutData& data = *layout;
  const auto render = [&](Out& out) {
    const views::ViewContext ctx = make_view_context(rq, data);
    views::LayoutParts parts;
    parts.content = [&](Out& o) { views::rooms::involvements::show(o, ctx, involvement); };
    render_in_layout(rq, data, parts, out);
  };
  co_return cached_page(rq, 200, deps, render);
}

Task<Flow<net::Response>> involvements_update(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto scoped = set_scoped_room(rq);
  if (!scoped) co_return std::unexpected(std::move(scoped.error()));
  // `params[:involvement]` as the enum casts it: a blank value is stored as nil, another value that is not an
  // involvement raises ArgumentError.
  std::optional<std::string> value;
  if (const req::Param* param = rq.params().get("involvement"); param != nullptr && !param->is_blank()) {
    const auto text = param->as_str();
    if (!text || !models::memberships::is_valid_involvement(*text)) {
      co_return fail_internal("'" + param->to_s().value_or("") + "' is not a valid involvement");
    }
    value = std::string(*text);
  }
  const std::optional<std::string> previous = scoped->membership.involvement;
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) {
    return models::memberships::update_involvement(tx, scoped->membership,
                                                   value ? std::optional<std::string_view>(*value) : std::nullopt);
  });
  if (!written) co_return fail_internal(written.error().message);

  // `broadcast_visibility_changes`
  const models::Room& room = scoped->room;
  const std::int64_t user_id = scoped->membership.user_id;
  if (!room.is_direct()) {
    if (scoped->membership.involvement == "invisible") {
      broadcasts::involvement_remove(rq.app, room, user_id);
    } else if (!previous) {
      // `nil.inquiry` raises NoMethodError after the update has saved.
      co_return fail_internal("undefined method 'inquiry' for nil");
    } else if (*previous == "invisible") {
      broadcasts::involvement_prepend(rq.app, user_id, shared_room_html(room));
    }
  }
  co_return rq.redirect_to(rq.url_for(campfire::routes::room_involvement(room.id)));
}

}  // namespace campfire::app::controllers

namespace campfire::routes::rooms_involvements {

Task<net::Response> show(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::involvements_show);
}
Task<net::Response> update(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::involvements_update);
}

}  // namespace campfire::routes::rooms_involvements
