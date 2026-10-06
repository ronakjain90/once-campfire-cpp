// Accounts::CustomStylesController. Rails: app/controllers/accounts/custom_styles_controller.rb. Rust: crates/
// campfire/src/controllers/accounts/custom_styles.rs.
#include "app/concerns.hpp"
#include "app/controllers/accounts_common.hpp"
#include "app/dispatch.hpp"
#include "models/account.hpp"
#include "models/account_admin.hpp"
#include "routes/routes.hpp"
#include "views/helpers/forms.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::controllers {

namespace {

// `before_action :ensure_can_administer, :set_account`
Task<Flow<net::Response>> custom_styles_edit(Rq& rq) {
  if (auto admin = co_await administrate(rq); !admin) co_return std::unexpected(std::move(admin.error()));
  db::DependencyScope& deps = rq.track();
  auto layout = load_layout(rq);
  if (!layout) co_return std::unexpected(std::move(layout.error()));
  auto account = models::accounts::first(rq.db(), rq.arena());
  if (!account) co_return fail_internal(account.error().message);
  if (!*account) co_return fail_internal("undefined method for nil (no account)");
  const req::Format offered[] = {&req::mime::HTML};
  if (auto format = rq.respond_to(offered); !format) co_return std::unexpected(std::move(format.error()));
  const std::optional<std::string> custom_styles = (*account)->custom_styles;
  // `form_with model: @account, url: account_custom_styles_url, ...`
  const views::helpers::FormWith form =
      views::helpers::form_with_url(rq.url_for(campfire::routes::account_custom_styles()))
          .model("account")
          .method("patch")
          .cls("flex flex-column gap")
          .data("controller", "form")
          .data("action", "keydown.ctrl+enter->form#submit keydown.meta+enter->form#submit");

  PageSpec spec;
  spec.name = "accounts/custom_styles#edit";
  spec.title = "Custom styles";
  spec.nav = [](Out& out, const views::ViewContext& ctx) { views::accounts::custom_styles::edit_nav(out, ctx); };
  spec.content = [&](Out& out, const views::ViewContext& ctx) {
    views::accounts::custom_styles::edit(out, ctx, custom_styles, form);
  };
  co_return render_tracked_page(rq, 200, deps, *layout, spec);
}

// `@account.update!(params.require(:account).permit(:custom_styles))`
Task<Flow<net::Response>> custom_styles_update(Rq& rq) {
  if (auto admin = co_await administrate(rq); !admin) co_return std::unexpected(std::move(admin.error()));
  auto account = models::accounts::first_edit(rq.db(), rq.arena());
  if (!account) co_return fail_internal(account.error().message);
  if (!*account) co_return fail_internal("undefined method for nil (no account)");
  const std::int64_t id = (*account)->id;
  auto required = rq.params().require("account");
  if (!required) co_return fail_with(ErrorKind::ParameterMissing, required.error().message);
  const req::ParamMap* hash = (*required)->as_hash();
  req::ParamMap params = hash == nullptr ? req::ParamMap(rq.ctx.resource()) : hash->permit({"custom_styles"}, rq.ctx.resource());
  models::accounts::Changes changes;
  if (params.contains("custom_styles")) changes.custom_styles = params.get("custom_styles")->to_s();
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(),
                                           [&](db::Tx& tx) -> Status { return models::accounts::update(tx, id, changes); });
  if (!written) co_return fail_internal(written.error().message);
  co_return redirect_to_path(rq, campfire::routes::edit_account_custom_styles(), std::string("\xE2\x9C\x93"));
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::accounts_custom_styles {

Task<net::Response> edit(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::custom_styles_edit);
}
Task<net::Response> update(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::custom_styles_update);
}

}  // namespace campfire::routes::accounts_custom_styles
