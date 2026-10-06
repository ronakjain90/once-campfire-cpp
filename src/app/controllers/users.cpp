// UsersController, the join actions. Rails: app/controllers/users_controller.rb (new, create). Rust: crates/campfire/src/controllers/users.rs.
#include "app/concerns.hpp"
#include "app/controllers/common.hpp"
#include "app/dispatch.hpp"
#include "app/render_page.hpp"
#include "compat/ruby.hpp"
#include "models/account.hpp"
#include "models/user.hpp"
#include "routes/routes.hpp"
#include "views/helpers/forms.hpp"
#include "views/sessions/helpers.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::controllers {

namespace {

// `head :not_found if Current.account.join_code != params[:join_code]`
Flow<void> verify_join_code(Rq& rq) {
  auto code = models::accounts::first_join_code(rq.db(), rq.arena());
  if (!code) return fail_internal(code.error().message);
  // `Current.account.join_code` on nil raises NoMethodError.
  if (!*code) return fail_internal("undefined method 'join_code' for nil");
  if (rq.param_str("join_code") != std::string_view(**code)) {
    return halt(concerns::head_in_before_action(rq, 404));
  }
  return {};
}

// `require_unauthenticated_access only: %i[ new create ]`, `before_action :verify_join_code, only: %i[ new create ]`
Task<Flow<net::Response>> users_new(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{}.require_unauthenticated_access());
  if (!before) co_return std::unexpected(std::move(before.error()));
  if (auto verified = verify_join_code(rq); !verified) co_return std::unexpected(std::move(verified.error()));
  const req::Format offered[] = {&req::mime::HTML};
  if (auto format = rq.respond_to(offered); !format) co_return std::unexpected(std::move(format.error()));
  auto owner = models::users::first_administrator(rq.db(), rq.arena());
  if (!owner) co_return fail_internal(owner.error().message);
  std::optional<views::HelpContact> help;
  if (*owner) help = views::HelpContact{(*owner)->name, (*owner)->email_address};
  const std::string join_code(rq.param_str("join_code").value_or(""));
  const views::helpers::FormWith form =
      views::helpers::form_with_url(campfire::routes::join(join_code)).model("user").cls("center");
  PageSpec spec;
  spec.name = "users#new";
  spec.title = "Sign up";
  spec.body_class = "signup";
  spec.nav = [](Out& out, const views::ViewContext& ctx) { views::users::new_nav(out, ctx); };
  spec.content = [&](Out& out, const views::ViewContext& ctx) { views::users::new_(out, ctx, form, help); };
  // The page prints the join code in the action of the form and the contact of the first administrator.
  spec.facets = [&](db::DependencyScope& deps, const LayoutData&) { deps.facet("join_code", join_code); };
  co_return render_page(rq, 200, spec);
}

Task<Flow<net::Response>> users_create(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{}.require_unauthenticated_access());
  if (!before) co_return std::unexpected(std::move(before.error()));
  if (auto verified = verify_join_code(rq); !verified) co_return std::unexpected(std::move(verified.error()));
  auto params = user_params(rq);
  if (!params) co_return std::unexpected(std::move(params.error()));
  const auto email_address = param_text(*params, "email_address");
  models::NewUser attributes;
  // users.name is NOT NULL: a missing name fails the insert, as in Rails.
  const auto name = param_text(*params, "name");
  if (!name) co_return fail_internal("NOT NULL constraint failed: users.name");
  attributes.name = *name;
  attributes.email_address = email_address;
  // The avatar upload needs Active Storage in the app (A6): it is not attached yet.
  auto digest = co_await concerns::password_digest(rq, param_text(*params, "password"));
  if (!digest) co_return std::unexpected(std::move(digest.error()));
  attributes.password_digest = std::move(*digest);

  // `User.create!(user_params)`
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(),
                                           [&](db::Tx& tx) -> Result<models::User> { return models::users::create(tx, attributes); });
  if (!written) {
    if (!is_record_not_unique(written.error())) co_return fail_internal(written.error().message);
    // rescue ActiveRecord::RecordNotUnique: `redirect_to new_session_url(email_address: user_params[:email_address])`
    std::string location = rq.url_for(campfire::routes::new_session());
    if (email_address) location += "?email_address=" + compat::cgi_escape(*email_address);
    co_return rq.redirect_to(location);
  }
  auto started = co_await concerns::start_new_session_for(rq, std::move(*written));
  if (!started) co_return std::unexpected(std::move(started.error()));
  co_return rq.redirect_to(rq.url_for(campfire::routes::root()));
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::join {

Task<net::Response> new_(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::users_new);
}
Task<net::Response> create(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::users_create);
}

}  // namespace campfire::routes::join
