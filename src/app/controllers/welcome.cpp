// WelcomeController#show. Rails: app/controllers/welcome_controller.rb, concerns/tracked_room_visit.rb. Rust:
// crates/campfire/src/controllers/welcome.rs.
#include "app/concerns.hpp"
#include "app/dispatch.hpp"
#include "app/render_page.hpp"
#include "compat/ruby.hpp"
#include "models/user_rooms.hpp"
#include "routes/routes.hpp"
#include "views/helpers/users.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::controllers {

namespace {

// `last_room_visited`: the room of the `last_room` cookie if the user is in it, else `Current.user.rooms.original`.
Flow<std::optional<std::int64_t>> last_room_visited(Rq& rq, std::int64_t user_id) {
  if (const auto cookie = rq.cookies().get("last_room")) {
    if (const auto room_id = compat::integer_cast(*cookie)) {
      auto room = models::user_rooms::find(rq.db(), rq.arena(), user_id, *room_id);
      if (!room) return fail_internal(room.error().message);
      if (*room) return *room;
    }
  }
  auto original = models::user_rooms::original(rq.db(), rq.arena(), user_id);
  if (!original) return fail_internal(original.error().message);
  return *original;
}

// To the last room visited, or a page that says there are no rooms yet.
Task<Flow<net::Response>> welcome_show(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  const models::User* user = rq.current_user();
  if (user == nullptr) co_return fail_internal("no current user");
  auto any = models::user_rooms::any(rq.db(), rq.arena(), user->id);
  if (!any) co_return fail_internal(any.error().message);
  if (*any) {
    // `redirect_to room_url(last_room_visited)`
    auto room = last_room_visited(rq, user->id);
    if (!room) co_return std::unexpected(std::move(room.error()));
    if (!*room) co_return fail_internal("no last room");
    co_return rq.redirect_to(rq.url_for(campfire::routes::room(**room)));
  }
  const req::Format offered[] = {&req::mime::HTML};
  if (auto format = rq.respond_to(offered); !format) co_return std::unexpected(std::move(format.error()));
  PageSpec spec;
  spec.name = "welcome#show";
  spec.title = "No rooms yet";
  spec.body_class = "sidebar";
  // The Rust wrapper layout prints the frame with no indent and no newline of its own: `</turbo-frame>    </aside>`.
  spec.sidebar = [](Out& out, const views::ViewContext&) {
    views::helpers::sidebar_turbo_frame_tag(out, campfire::routes::user_sidebar());
  };
  spec.content = [](Out& out, const views::ViewContext& ctx) { views::welcome::show(out, ctx); };
  co_return render_page(rq, 200, spec);
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::welcome {

Task<net::Response> show(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::welcome_show);
}

}  // namespace campfire::routes::welcome
