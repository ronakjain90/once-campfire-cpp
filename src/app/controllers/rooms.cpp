// RoomsController and its subclasses. Rails: app/controllers/rooms_controller.rb, app/controllers/rooms/*.rb,
// app/controllers/concerns/tracked_room_visit.rb. Rust: crates/campfire/src/controllers/rooms.rs.
#include "app/controllers/rooms.hpp"

#include <charconv>
#include <string>

#include "app/concerns.hpp"
#include "app/dispatch.hpp"
#include "routes/routes.hpp"

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

// `redirect_to room_url(Current.user.rooms.last)`
Task<Flow<net::Response>> rooms_index(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto last = models::rooms::last_of_user(rq.db(), rq.arena(), rq.current_user()->id);
  if (!last) co_return fail_internal(last.error().message);
  // `room_url(nil)` raises ActionController::UrlGenerationError.
  if (!*last) co_return fail_internal("No route matches {:controller=>\"rooms\", :action=>\"show\"}");
  co_return rq.redirect_to(rq.url_for(campfire::routes::room((*last)->id)));
}

namespace {

// `Rooms::OpensController#show` and `Rooms::ClosedsController#show`: `redirect_to room_url(@room)`.
Task<Flow<net::Response>> redirect_to_room(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto room = set_room(rq, models::RoomScope::WithoutDirects);
  if (!room) co_return std::unexpected(std::move(room.error()));
  remember_last_room_visited(rq, *room);
  co_return rq.redirect_to(rq.url_for(campfire::routes::room(room->id)));
}

}  // namespace

Task<Flow<net::Response>> opens_show(Rq& rq) {
  return redirect_to_room(rq);
}

Task<Flow<net::Response>> closeds_show(Rq& rq) {
  return redirect_to_room(rq);
}

}  // namespace campfire::app::controllers

namespace campfire::routes::rooms {

Task<net::Response> index(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::rooms_index);
}

}  // namespace campfire::routes::rooms

namespace campfire::routes::rooms_opens {

Task<net::Response> show(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::opens_show);
}

}  // namespace campfire::routes::rooms_opens

namespace campfire::routes::rooms_closeds {

Task<net::Response> show(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::closeds_show);
}

}  // namespace campfire::routes::rooms_closeds
