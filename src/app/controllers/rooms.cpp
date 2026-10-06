// RoomsController and its subclasses. Rails: app/controllers/rooms_controller.rb, app/controllers/rooms/*.rb,
// app/controllers/concerns/tracked_room_visit.rb. Rust: crates/campfire/src/controllers/rooms.rs.
#include "app/controllers/rooms.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <functional>
#include <string>

#include "app/broadcasts.hpp"
#include "app/concerns.hpp"
#include "app/controllers/sidebars.hpp"
#include "app/dispatch.hpp"
#include "app/page.hpp"
#include "models/account.hpp"
#include "models/membership.hpp"
#include "models/user.hpp"
#include "routes/routes.hpp"
#include "views/rooms/pages.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::controllers {

std::optional<std::int64_t> cast_id(std::string_view text) {
  // `"12abc".to_i`: leading blanks, an optional sign, then digits.
  while (!text.empty() && (text.front() == ' ' || (text.front() >= '\t' && text.front() <= '\r'))) {
    text.remove_prefix(1);
  }
  if (!text.empty() && text.front() == '+') text.remove_prefix(1);
  std::size_t digits = text.empty() || text.front() == '-' ? 1 : 0;
  while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') ++digits;
  const std::string_view number = text.substr(0, digits);
  if (number.empty() || number == "-") return std::nullopt;
  std::int64_t value = 0;
  const auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), value);
  if (ec != std::errc{} || end != number.data() + number.size()) return std::nullopt;
  return value;
}

Flow<models::Room> set_room(Rq& rq, models::RoomScope scope) {
  const models::User* user = rq.current_user();
  if (user == nullptr) return fail_internal("set_room without a user");
  // `params[:room_id] || params[:id]`
  std::optional<std::string_view> raw = rq.param_str("room_id");
  if (!raw) raw = rq.param_str("id");
  const std::optional<std::int64_t> id = raw ? cast_id(*raw) : std::nullopt;
  if (id) {
    auto found = models::rooms::find_for_user(rq.db(), rq.arena(), user->id, scope, *id);
    if (!found) return fail_internal(found.error().message);
    if (*found) return std::move(**found);
  }
  RedirectOptions options;
  options.alert = "Room not found or inaccessible";
  auto redirect = rq.redirect_to(rq.url_for(campfire::routes::root()), std::move(options));
  if (!redirect) return std::unexpected(std::move(redirect.error()));
  return halt(std::move(*redirect));
}

void remember_last_room_visited(Rq& rq, const models::Room& room) {
  req::Cookie cookie;
  cookie.value = std::to_string(room.id);
  cookie.permanent = true;
  rq.cookies().set("last_room", std::move(cookie));
}

Flow<void> ensure_html(Rq& rq) {
  const req::Format offered[] = {&req::mime::HTML};
  if (auto format = rq.respond_to(offered); !format) return std::unexpected(std::move(format.error()));
  return {};
}

Flow<net::Response> redirect_to_room(Rq& rq, std::int64_t room_id) {
  return rq.redirect_to(rq.url_for(campfire::routes::room(room_id)));
}

Flow<net::Response> redirect_to_root(Rq& rq) {
  return rq.redirect_to(rq.url_for(campfire::routes::root()));
}

Flow<void> ensure_can_administer(Rq& rq, const models::Room& room) {
  const models::User* user = rq.current_user();
  if (user == nullptr || !user->can_administer(room.creator_id)) return halt(concerns::head_in_before_action(rq, 403));
  return {};
}

Flow<void> ensure_permission_to_create_rooms(Rq& rq) {
  const models::User* user = rq.current_user();
  auto account = models::accounts::first(rq.db(), rq.arena());
  if (!account) return fail_internal(account.error().message);
  const bool restricted = *account && (*account)->restrict_room_creation_to_administrators;
  if (restricted && (user == nullptr || !user->is_administrator()))
    return halt(concerns::head_in_before_action(rq, 403));
  return {};
}

Flow<std::optional<std::optional<std::string>>> room_name_param(Rq& rq) {
  auto room = rq.params().require("room");
  if (!room) return fail_with(ErrorKind::ParameterMissing, room.error().message);
  const req::Param* name = (*room)->get("name");
  // `permit(:name)` keeps a scalar and drops anything else.
  if (name == nullptr) return std::optional<std::optional<std::string>>{};
  if (name->is_null()) return std::optional<std::optional<std::string>>(std::optional<std::string>{});
  if (const auto text = name->as_str()) {
    return std::optional<std::optional<std::string>>(std::optional<std::string>(std::string(*text)));
  }
  return std::optional<std::optional<std::string>>{};
}

std::vector<std::int64_t> user_ids_param(Rq& rq) {
  std::vector<std::int64_t> ids;
  const req::Param* param = rq.params().get("user_ids");
  if (param == nullptr) return ids;
  if (const req::ParamArray* array = param->as_array()) {
    for (const req::Param& item : *array) {
      if (const auto text = item.as_str()) {
        if (const auto id = cast_id(*text)) ids.push_back(*id);
      }
    }
  } else if (const auto text = param->as_str()) {
    if (const auto id = cast_id(*text)) ids.push_back(*id);
  }
  return ids;
}

std::string shared_room_html(const models::Room& room) {
  views::SidebarRoom shared;
  shared.id = room.id;
  shared.param_key = room.is_open() ? "rooms_open" : "rooms_closed";
  shared.name = room.name.value_or("");
  Out out;
  views::users::sidebars::rooms::shared(out, shared);
  return out.to_string();
}

Task<Flow<net::Response>> destroy_room(Rq& rq, models::Room room) {
  auto destroyed =
      co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) { return models::rooms::destroy(tx, room); });
  if (!destroyed) co_return fail_internal(destroyed.error().message);
  // `broadcast_remove_to :rooms, target: [ @room, :list ]`
  broadcasts::room_remove(rq.app, room);
  co_return redirect_to_root(rq);
}

namespace {

// The users that a form lists, as the views see them.
Flow<std::vector<views::UserSummary>> active_users(Rq& rq) {
  auto users = models::users::active_ordered(rq.db(), rq.arena());
  if (!users) return fail_internal(users.error().message);
  std::vector<views::UserSummary> out;
  out.reserve(users->size());
  for (const models::User& user : *users) out.push_back(user_summary(rq, user));
  return out;
}

using FillPage = std::function<void(views::LayoutParts&, std::string&, const views::ViewContext&)>;

// A page in the application layout, or in the frame layout for a Turbo-Frame request, through the page cache.
Flow<net::Response> render_page(Rq& rq, std::string_view name, db::DependencyScope& deps, const LayoutData& data,
                                const FillPage& fill) {
  // The implicit render finds only an HTML template: `Accept: application/json` is `UnknownFormat` (406).
  if (auto format = ensure_html(rq); !format) return std::unexpected(std::move(format.error()));
  add_page_facets(rq, deps, name);
  const auto render = [&](Out& out) {
    const views::ViewContext ctx = make_view_context(rq, data);
    views::LayoutParts parts;
    std::string title;
    fill(parts, title, ctx);
    render_in_layout(rq, data, parts, out);
  };
  return cached_page(rq, 200, deps, render);
}

// `broadcast_create_room` and `broadcast_update_room` of Closeds: the partial is rendered once for all members.
Flow<void> broadcast_to_members(Rq& rq, const models::Room& room, bool update) {
  auto members = models::rooms::user_ids(rq.db(), rq.arena(), room.id);
  if (!members) return fail_internal(members.error().message);
  const std::string html = shared_room_html(room);
  if (update) {
    broadcasts::closed_room_update(rq.app, room, *members, html);
  } else {
    broadcasts::closed_room_create(rq.app, *members, html);
  }
  return {};
}

constexpr std::string_view kDefaultRoomName = "New room";

Flow<std::vector<std::int64_t>> existing_user_ids(Rq& rq, const std::vector<std::int64_t>& ids) {
  auto existing = models::users::existing_ids(rq.db(), rq.arena(), ids);
  if (!existing) return fail_internal(existing.error().message);
  return std::move(*existing);
}

std::optional<std::string_view> view_of(const std::optional<std::string>& text) {
  return text ? std::optional<std::string_view>(*text) : std::nullopt;
}

}  // namespace

// `redirect_to room_url(Current.user.rooms.last)`
Task<Flow<net::Response>> rooms_index(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto last = models::rooms::last_of_user(rq.db(), rq.arena(), rq.current_user()->id);
  if (!last) co_return fail_internal(last.error().message);
  // `room_url(nil)` raises ActionController::UrlGenerationError.
  if (!*last) co_return fail_internal("No route matches {:controller=>\"rooms\", :action=>\"show\"}");
  co_return redirect_to_room(rq, (*last)->id);
}

Task<Flow<net::Response>> rooms_destroy(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto room = set_room(rq, models::RoomScope::All);
  if (!room) co_return std::unexpected(std::move(room.error()));
  if (auto allowed = ensure_can_administer(rq, *room); !allowed) co_return std::unexpected(std::move(allowed.error()));
  co_return co_await destroy_room(rq, std::move(*room));
}

Task<Flow<net::Response>> action_not_found(Rq&) {
  co_return fail_with(ErrorKind::NotFound, "The action could not be found");
}

Task<Flow<net::Response>> missing_controller(Rq&) {
  co_return fail_internal("uninitialized constant Rooms::SettingsController");
}

Task<Flow<net::Response>> destroy_without_room(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  co_return fail_internal("undefined method 'destroy' for nil");
}

namespace {

// `Rooms::OpensController#show` and `Rooms::ClosedsController#show`: `redirect_to room_url(@room)`.
Task<Flow<net::Response>> redirect_to_room_action(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto room = set_room(rq, models::RoomScope::WithoutDirects);
  if (!room) co_return std::unexpected(std::move(room.error()));
  remember_last_room_visited(rq, *room);
  co_return redirect_to_room(rq, room->id);
}

}  // namespace

Task<Flow<net::Response>> opens_show(Rq& rq) {
  return redirect_to_room_action(rq);
}

Task<Flow<net::Response>> closeds_show(Rq& rq) {
  return redirect_to_room_action(rq);
}

Task<Flow<net::Response>> opens_new(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  if (auto allowed = ensure_permission_to_create_rooms(rq); !allowed)
    co_return std::unexpected(std::move(allowed.error()));
  db::DependencyScope& deps = rq.track();
  auto layout = load_layout(rq);
  if (!layout) co_return std::unexpected(std::move(layout.error()));
  views::OpenFormView form;
  form.room.name = std::string(kDefaultRoomName);
  form.can_administer = true;
  auto users = active_users(rq);
  if (!users) co_return std::unexpected(std::move(users.error()));
  form.users = std::move(*users);
  co_return render_page(rq, "rooms/opens#new", deps, *layout,
                        [&](views::LayoutParts& parts, std::string& title, const views::ViewContext& ctx) {
                          views::rooms::opens_new(parts, title, ctx, form);
                        });
}

Task<Flow<net::Response>> opens_create(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  if (auto allowed = ensure_permission_to_create_rooms(rq); !allowed)
    co_return std::unexpected(std::move(allowed.error()));
  auto name = room_name_param(rq);
  if (!name) co_return std::unexpected(std::move(name.error()));
  const std::int64_t user_id = rq.current_user()->id;
  const std::optional<std::string> room_name = name->value_or(std::nullopt);
  std::optional<models::Room> created;
  // `Rooms::Open.create_for(room_params, users: Current.user)`
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Status {
    const std::array<std::int64_t, 1> members{user_id};
    auto room = models::rooms::create_for(tx, models::kRoomOpen, view_of(room_name), user_id, members);
    if (!room) return std::unexpected(room.error());
    created = std::move(*room);
    return {};
  });
  if (!written) co_return fail_internal(written.error().message);
  // `broadcast_prepend_to :rooms, target: :shared_rooms, partial: "users/sidebars/rooms/shared"`
  broadcasts::open_room_create(rq.app, shared_room_html(*created));
  co_return redirect_to_room(rq, created->id);
}

Task<Flow<net::Response>> opens_edit(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  db::DependencyScope& deps = rq.track();
  auto room = set_room(rq, models::RoomScope::WithoutDirects);
  if (!room) co_return std::unexpected(std::move(room.error()));
  auto layout = load_layout(rq);
  if (!layout) co_return std::unexpected(std::move(layout.error()));
  views::OpenFormView form;
  form.room.id = room->id;
  form.room.name = room->name;
  form.can_administer = rq.current_user()->can_administer(room->creator_id);
  auto users = active_users(rq);
  if (!users) co_return std::unexpected(std::move(users.error()));
  form.users = std::move(*users);
  co_return render_page(rq, "rooms/opens#edit", deps, *layout,
                        [&](views::LayoutParts& parts, std::string& title, const views::ViewContext& ctx) {
                          views::rooms::opens_edit(parts, title, ctx, form);
                        });
}

Task<Flow<net::Response>> opens_update(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto found = set_room(rq, models::RoomScope::WithoutDirects);
  if (!found) co_return std::unexpected(std::move(found.error()));
  models::Room room = std::move(*found);
  if (auto allowed = ensure_can_administer(rq, room); !allowed) co_return std::unexpected(std::move(allowed.error()));
  auto name = room_name_param(rq);
  if (!name) co_return std::unexpected(std::move(name.error()));
  // `force_room_type`, then `@room.update! room_params`.
  std::optional<std::optional<std::string_view>> change;
  if (*name) change = view_of(**name);
  auto written = co_await rq.app.db->write(
      rq.ctx.scheduler(), [&](db::Tx& tx) { return models::rooms::update(tx, room, change, models::kRoomOpen); });
  if (!written) co_return fail_internal(written.error().message);
  broadcasts::open_room_update(rq.app, room, shared_room_html(room));
  co_return redirect_to_room(rq, room.id);
}

Task<Flow<net::Response>> closeds_new(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  if (auto allowed = ensure_permission_to_create_rooms(rq); !allowed)
    co_return std::unexpected(std::move(allowed.error()));
  db::DependencyScope& deps = rq.track();
  auto layout = load_layout(rq);
  if (!layout) co_return std::unexpected(std::move(layout.error()));
  views::ClosedFormView form;
  form.room.name = std::string(kDefaultRoomName);
  form.can_administer = true;
  form.current_user_id = rq.current_user()->id;
  // `@users = User.active.ordered`; the form shows them all as unselected.
  auto users = active_users(rq);
  if (!users) co_return std::unexpected(std::move(users.error()));
  form.unselected_users = std::move(*users);
  co_return render_page(rq, "rooms/closeds#new", deps, *layout,
                        [&](views::LayoutParts& parts, std::string& title, const views::ViewContext& ctx) {
                          views::rooms::closeds_new(parts, title, ctx, form);
                        });
}

Task<Flow<net::Response>> closeds_create(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  if (auto allowed = ensure_permission_to_create_rooms(rq); !allowed)
    co_return std::unexpected(std::move(allowed.error()));
  auto name = room_name_param(rq);
  if (!name) co_return std::unexpected(std::move(name.error()));
  const std::int64_t user_id = rq.current_user()->id;
  const std::optional<std::string> room_name = name->value_or(std::nullopt);
  // `Rooms::Closed.create_for(room_params, users: grantees)`
  auto grantees = existing_user_ids(rq, user_ids_param(rq));
  if (!grantees) co_return std::unexpected(std::move(grantees.error()));
  std::optional<models::Room> created;
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Status {
    auto room = models::rooms::create_for(tx, models::kRoomClosed, view_of(room_name), user_id, *grantees);
    if (!room) return std::unexpected(room.error());
    created = std::move(*room);
    return {};
  });
  if (!written) co_return fail_internal(written.error().message);
  if (auto sent = broadcast_to_members(rq, *created, false); !sent) co_return std::unexpected(std::move(sent.error()));
  co_return redirect_to_room(rq, created->id);
}

Task<Flow<net::Response>> closeds_edit(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  db::DependencyScope& deps = rq.track();
  auto room = set_room(rq, models::RoomScope::WithoutDirects);
  if (!room) co_return std::unexpected(std::move(room.error()));
  auto layout = load_layout(rq);
  if (!layout) co_return std::unexpected(std::move(layout.error()));
  const models::User& user = *rq.current_user();
  views::ClosedFormView form;
  form.room.id = room->id;
  form.room.name = room->name;
  form.can_administer = user.can_administer(room->creator_id);
  form.current_user_id = user.id;
  // `@selected_users, @unselected_users = User.active.ordered.partition { |user| selected_user_ids.include?(user.id) }`
  auto selected_ids = models::rooms::user_ids(rq.db(), rq.arena(), room->id);
  if (!selected_ids) co_return fail_internal(selected_ids.error().message);
  auto users = models::users::active_ordered(rq.db(), rq.arena());
  if (!users) co_return fail_internal(users.error().message);
  for (const models::User& candidate : *users) {
    const bool selected = std::find(selected_ids->begin(), selected_ids->end(), candidate.id) != selected_ids->end();
    (selected ? form.selected_users : form.unselected_users).push_back(user_summary(rq, candidate));
  }
  co_return render_page(rq, "rooms/closeds#edit", deps, *layout,
                        [&](views::LayoutParts& parts, std::string& title, const views::ViewContext& ctx) {
                          views::rooms::closeds_edit(parts, title, ctx, form);
                        });
}

Task<Flow<net::Response>> closeds_update(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto found = set_room(rq, models::RoomScope::WithoutDirects);
  if (!found) co_return std::unexpected(std::move(found.error()));
  models::Room room = std::move(*found);
  if (auto allowed = ensure_can_administer(rq, room); !allowed) co_return std::unexpected(std::move(allowed.error()));
  auto name = room_name_param(rq);
  if (!name) co_return std::unexpected(std::move(name.error()));
  const std::vector<std::int64_t> grantee_ids = user_ids_param(rq);
  std::optional<std::optional<std::string_view>> change;
  if (*name) change = view_of(**name);
  // `force_room_type`, `@room.update! room_params`, then `@room.memberships.revise(granted:, revoked:)`.
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Status {
    if (auto done = models::rooms::update(tx, room, change, models::kRoomClosed); !done) return done;
    Arena arena(1024);
    auto granted = models::users::existing_ids(tx.conn(), arena, grantee_ids);
    if (!granted) return std::unexpected(granted.error());
    auto members = models::rooms::user_ids(tx.conn(), arena, room.id);
    if (!members) return std::unexpected(members.error());
    std::vector<std::int64_t> revoked;
    for (const std::int64_t member : *members) {
      if (std::find(grantee_ids.begin(), grantee_ids.end(), member) == grantee_ids.end()) revoked.push_back(member);
    }
    return models::rooms::revise(tx, room, *granted, revoked);
  });
  if (!written) co_return fail_internal(written.error().message);
  if (auto sent = broadcast_to_members(rq, room, true); !sent) co_return std::unexpected(std::move(sent.error()));
  co_return redirect_to_room(rq, room.id);
}

}  // namespace campfire::app::controllers

namespace campfire::routes::rooms {

Task<net::Response> index(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::rooms_index);
}
Task<net::Response> show(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::rooms_show);
}
Task<net::Response> destroy(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::rooms_destroy);
}
Task<net::Response> destroy_without_room(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::destroy_without_room);
}
Task<net::Response> action_not_found(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::action_not_found);
}
Task<net::Response> missing_controller(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::missing_controller);
}

}  // namespace campfire::routes::rooms

namespace campfire::routes::rooms_opens {

Task<net::Response> show(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::opens_show);
}
Task<net::Response> new_(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::opens_new);
}
Task<net::Response> create(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::opens_create);
}
Task<net::Response> edit(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::opens_edit);
}
Task<net::Response> update(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::opens_update);
}

}  // namespace campfire::routes::rooms_opens

namespace campfire::routes::rooms_closeds {

Task<net::Response> show(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::closeds_show);
}
Task<net::Response> new_(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::closeds_new);
}
Task<net::Response> create(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::closeds_create);
}
Task<net::Response> edit(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::closeds_edit);
}
Task<net::Response> update(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::closeds_update);
}

}  // namespace campfire::routes::rooms_closeds

namespace campfire::app::controllers {

// `show`, until the messages exist (A3): the checks and the cookie of the real action, then a 404.
Task<Flow<net::Response>> rooms_show(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto room = set_room(rq, models::RoomScope::All);
  if (!room) co_return std::unexpected(std::move(room.error()));
  remember_last_room_visited(rq, *room);
  co_return fail_with(ErrorKind::NotFound, "rooms#show waits for the messages (A3)");
}

}  // namespace campfire::app::controllers
