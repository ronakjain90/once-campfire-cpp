// UsersController#show. Rails: app/controllers/users_controller.rb (join: A1, in users.cpp). Rust: crates/campfire/src/
// controllers/users.rs (show), presenters/accounts.rs (transfer_id).
#include "app/concerns.hpp"
#include "app/controllers/accounts_common.hpp"
#include "app/controllers/sidebars.hpp"
#include "app/dispatch.hpp"
#include "models/user_admin.hpp"
#include "routes/routes.hpp"
#include "views/accounts/types.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::controllers {

namespace {

// `before_action :set_user`: `User.find(params[:id])`.
Task<Flow<net::Response>> users_show(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  db::DependencyScope& deps = rq.track();
  auto layout = load_layout(rq);
  if (!layout) co_return std::unexpected(std::move(layout.error()));
  const auto id = id_param(rq, "id");
  if (!id) co_return fail_with(ErrorKind::NotFound, "Couldn't find User");
  auto found = models::users::find_by_id(rq.db(), rq.arena(), *id);
  if (!found) co_return fail_internal(found.error().message);
  if (!*found) co_return fail_with(ErrorKind::NotFound, "Couldn't find User");
  const models::User& user = **found;
  const req::Format offered[] = {&req::mime::HTML};
  if (auto format = rq.respond_to(offered); !format) co_return std::unexpected(std::move(format.error()));

  views::UserShowView view;
  view.user = user_summary(rq, user);
  view.email_address = user.email_address;
  view.bio = user.bio;
  view.bot = user.is_bot();
  view.active = user.status == models::kStatusActive;
  view.deactivated = user.status == models::kStatusDeactivated;
  view.banned = user.status == models::kStatusBanned;
  view.is_current = rq.current_user()->id == user.id;
  // The transfer link is on the page for an administrator, for an active user that is not a bot.
  const bool administrator = rq.current_user()->can_administer();
  if (administrator && view.active && !view.bot) {
    view.transfer_url = rq.url_for(campfire::routes::session_transfer(transfer_id(rq, user.id)));
    deps.mark_uncacheable();  // the expiry of the link is in it
  }

  PageSpec spec;
  spec.name = "users#show";
  spec.title = view.user.name;
  spec.nav = [&](Out& out, const views::ViewContext& ctx) { views::users::show_nav(out, ctx, view.is_current); };
  spec.content = [&](Out& out, const views::ViewContext& ctx) { views::users::show(out, ctx, view); };
  spec.facets = [&](db::DependencyScope& d, const LayoutData&) {
    add_link_back_facets(rq, d);
    d.facet("shown_user", static_cast<std::uint64_t>(user.id));
    d.facet("shown_user_avatar", view.user.avatar_path);
  };
  co_return render_tracked_page(rq, 200, deps, *layout, spec);
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::users {

Task<net::Response> show(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::users_show);
}

}  // namespace campfire::routes::users
