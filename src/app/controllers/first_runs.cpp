// FirstRunsController. Rails: app/controllers/first_runs_controller.rb. Rust:
// crates/campfire/src/controllers/first_runs.rs.
#include "app/concerns.hpp"
#include "app/controllers/common.hpp"
#include "app/dispatch.hpp"
#include "app/render_page.hpp"
#include "models/account.hpp"
#include "models/first_run.hpp"
#include "routes/routes.hpp"
#include "views/helpers/forms.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::controllers {

namespace {

constexpr std::string_view kTitle = "Set up Campfire";

// `redirect_to root_url if Account.any?`
Flow<void> prevent_repeats(Rq& rq) {
  auto any = models::accounts::any(rq.db(), rq.arena());
  if (!any) return fail_internal(any.error().message);
  if (!*any) return {};
  auto redirect = rq.redirect_to(rq.url_for(campfire::routes::root()));
  if (!redirect) return std::unexpected(std::move(redirect.error()));
  return halt(std::move(*redirect));
}

// `allow_unauthenticated_access`, `before_action :prevent_repeats`
Task<Flow<net::Response>> first_runs_show(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{}.allow_unauthenticated_access());
  if (!before) co_return std::unexpected(std::move(before.error()));
  if (auto repeat = prevent_repeats(rq); !repeat) co_return std::unexpected(std::move(repeat.error()));
  const req::Format offered[] = {&req::mime::HTML};
  if (auto format = rq.respond_to(offered); !format) co_return std::unexpected(std::move(format.error()));
  const views::helpers::FormWith form =
      views::helpers::form_with_url(campfire::routes::first_run()).model("user").cls("center max-width");
  PageSpec spec;
  spec.name = "first_runs#show";
  spec.title = kTitle;
  spec.body_class = "signup";
  spec.content = [&](Out& out, const views::ViewContext& ctx) { views::first_runs::show(out, ctx, form, kTitle); };
  co_return render_page(rq, 200, spec);
}

Task<Flow<net::Response>> first_runs_create(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{}.allow_unauthenticated_access());
  if (!before) co_return std::unexpected(std::move(before.error()));
  if (auto repeat = prevent_repeats(rq); !repeat) co_return std::unexpected(std::move(repeat.error()));

  // `params.require(:user).permit(:name, :avatar, :email_address, :password)`
  auto params = user_params(rq);
  if (!params) co_return std::unexpected(std::move(params.error()));
  const auto name = param_text(*params, "name");
  // users.name is NOT NULL: Rails raises ActiveRecord::NotNullViolation.
  if (!name) co_return fail_internal("NOT NULL constraint failed: users.name");
  const auto email_address = param_text(*params, "email_address");
  // The avatar upload needs Active Storage in the app (A6): it is not attached yet.
  auto digest = co_await concerns::password_digest(rq, param_text(*params, "password"));
  if (!digest) co_return std::unexpected(std::move(digest.error()));

  auto written = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Result<models::User> {
    return models::first_run::create(tx, *name, email_address, std::move(*digest));
  });
  // rescue ActiveRecord::RecordNotUnique
  if (!written && is_record_not_unique(written.error())) {
    co_return rq.redirect_to(rq.url_for(campfire::routes::root()));
  }
  if (!written) co_return fail_internal(written.error().message);
  auto started = co_await concerns::start_new_session_for(rq, std::move(*written));
  if (!started) co_return std::unexpected(std::move(started.error()));
  co_return rq.redirect_to(rq.url_for(campfire::routes::root()));
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::first_runs {

Task<net::Response> show(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::first_runs_show);
}
Task<net::Response> create(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::first_runs_create);
}

}  // namespace campfire::routes::first_runs
