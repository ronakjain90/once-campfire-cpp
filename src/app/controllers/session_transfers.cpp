// Sessions::TransfersController. Rails: app/controllers/sessions/transfers_controller.rb. Rust:
// crates/campfire/src/controllers/sessions/transfers.rs.
#include "app/concerns.hpp"
#include "app/dispatch.hpp"
#include "app/render_page.hpp"
#include "compat/signed_id.hpp"
#include "models/user.hpp"
#include "views/helpers/forms.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::controllers {

namespace {

// `allow_unauthenticated_access`: an auto-submit form that sends PUT to this URL.
Task<Flow<net::Response>> transfers_show(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{}.allow_unauthenticated_access());
  if (!before) co_return std::unexpected(std::move(before.error()));
  const req::Format offered[] = {&req::mime::HTML};
  if (auto format = rq.respond_to(offered); !format) co_return std::unexpected(std::move(format.error()));
  // `url_for({})`: the path of this request.
  const std::string action(rq.request.path);
  const views::helpers::FormWith form = views::helpers::form_with_url(action).method("put").auto_submit();
  PageSpec spec;
  spec.name = "sessions/transfers#show";
  spec.content = [&](Out& out, const views::ViewContext&) { views::sessions::transfers::show(out, form); };
  spec.facets = [&](db::DependencyScope& deps, const LayoutData&) { deps.facet("action", action); };
  co_return render_page(rq, 200, spec);
}

Task<Flow<net::Response>> transfers_update(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{}.allow_unauthenticated_access());
  if (!before) co_return std::unexpected(std::move(before.error()));
  // `User.active.find_by_transfer_id(params[:id])`
  const Timestamp at = rq.now();
  const compat::Timestamp now(std::chrono::nanoseconds(at.seconds * 1'000'000'000LL + at.nanos));
  const std::string_view transfer_id = rq.param_str("id").value_or("");
  std::optional<models::User> user;
  if (const auto id = compat::signed_id::verify(rq.app.secrets, "User", transfer_id, "transfer", now)) {
    auto found = models::users::find_by_id(rq.db(), rq.arena(), *id);
    if (!found) co_return fail_internal(found.error().message);
    if (*found && (*found)->status == models::kStatusActive) user = std::move(**found);
  }
  if (!user) co_return rq.head(400);
  auto started = co_await concerns::start_new_session_for(rq, std::move(*user));
  if (!started) co_return std::unexpected(std::move(started.error()));
  co_return rq.redirect_to(concerns::post_authenticating_url(rq));
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::session_transfers {

Task<net::Response> show(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::transfers_show);
}
Task<net::Response> update(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::transfers_update);
}

}  // namespace campfire::routes::session_transfers
