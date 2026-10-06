// AccountsController. Rails: app/controllers/accounts_controller.rb. Rust: crates/campfire/src/controllers/accounts.rs.
#include "app/active_storage.hpp"
#include "app/concerns.hpp"
#include "app/controllers/accounts_common.hpp"
#include "app/controllers/accounts_users_list.hpp"
#include "app/dispatch.hpp"
#include "models/account.hpp"
#include "models/account_admin.hpp"
#include "models/attachments.hpp"
#include "models/user_admin.hpp"
#include "routes/routes.hpp"
#include "views/accounts/types.hpp"
#include "views/helpers/forms.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::controllers {

namespace {

// `Current.account` where the reference dereferences it: a nil account raises NoMethodError.
constexpr std::string_view kNoAccount = "undefined method for nil (no account)";

// `before_action :set_account` and `edit`.
Task<Flow<net::Response>> accounts_edit(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  db::DependencyScope& deps = rq.track();
  auto layout = load_layout(rq);
  if (!layout) co_return std::unexpected(std::move(layout.error()));
  auto account = models::accounts::first_edit(rq.db(), rq.arena());
  if (!account) co_return fail_internal(account.error().message);
  if (!*account) co_return fail_internal(std::string(kNoAccount));
  auto restricted = models::accounts::first(rq.db(), rq.arena());
  if (!restricted) co_return fail_internal(restricted.error().message);
  const req::Format offered[] = {&req::mime::HTML};
  if (auto format = rq.respond_to(offered); !format) co_return std::unexpected(std::move(format.error()));

  // `account_users.ordered.without_bots`: an administrator also sees the banned users.
  const bool can_administer = rq.current_user()->can_administer();
  auto users = models::users::account_users(rq.db(), rq.arena(), can_administer);
  if (!users) co_return fail_internal(users.error().message);
  const Page page = Page::from(rq.param_str("page"), static_cast<std::int64_t>(users->size()));

  views::AccountEditView view;
  view.account_id = (*account)->id;
  view.join_code = (*account)->join_code;
  view.restrict_room_creation_to_administrators =
      *restricted && (*restricted)->restrict_room_creation_to_administrators;
  for (const models::User& user : *users) {
    views::AccountUser row = account_user(rq, user);
    (user.is_administrator() ? view.administrators : view.members).push_back(std::move(row));
  }
  if (!page.is_last()) view.next_page = std::to_string(page.next_param());

  // `form_with model: @account, method: :patch ...`: four forms, the first two with the logo field.
  // `form_with model: @account`: the singular route takes the record as its format, `/account.1`.
  const std::string action = campfire::routes::account() + "." + std::to_string(view.account_id);
  const views::helpers::FormWith logo_form = views::helpers::form_with_url(action)
                                                 .model("account")
                                                 .method("patch")
                                                 .cls("txt--medium")
                                                 .data("controller", "form");
  const views::helpers::FormWith image_form =
      views::helpers::form_with_url(action).model("account").method("patch").data("controller", "form");
  const views::helpers::FormWith name_form = views::helpers::form_with_url(action)
                                                 .model("account")
                                                 .method("patch")
                                                 .data("controller", "form")
                                                 .cls("flex flex-column gap");
  const views::helpers::FormWith settings_form = views::helpers::form_with_url(action)
                                                     .model("account")
                                                     .method("put")
                                                     .data("controller", "form")
                                                     .cls("flex align-center gap center");
  const views::helpers::FormWith settings_fields = settings_form.fields_for("settings");
  const std::string join_url = rq.url_for(campfire::routes::join(view.join_code));

  PageSpec spec;
  spec.name = "accounts#edit";
  spec.title = "Account settings";
  spec.nav = [](Out& out, const views::ViewContext& ctx) { views::accounts::edit_nav(out, ctx); };
  spec.content = [&](Out& out, const views::ViewContext& ctx) {
    views::accounts::edit(out, ctx, view, join_url, logo_form, image_form, name_form, settings_form, settings_fields);
  };
  spec.footer = [](Out& out, const views::ViewContext& ctx) { views::accounts::edit_footer(out, ctx); };
  co_return render_tracked_page(rq, 200, deps, *layout, spec);
}

// `@account.update!(params.require(:account).permit(:name, :logo, settings: {}))`
Task<Flow<net::Response>> accounts_update(Rq& rq) {
  if (auto admin = co_await administrate(rq); !admin) co_return std::unexpected(std::move(admin.error()));
  auto account = models::accounts::first_edit(rq.db(), rq.arena());
  if (!account) co_return fail_internal(account.error().message);
  if (!*account) co_return fail_internal(std::string(kNoAccount));
  const std::int64_t account_id = (*account)->id;

  auto required = rq.params().require("account");
  if (!required) co_return fail_with(ErrorKind::ParameterMissing, required.error().message);
  const req::ParamMap* hash = (*required)->as_hash();
  req::ParamMap params = hash == nullptr
                             ? req::ParamMap(rq.ctx.resource())
                             : hash->permit({"name", "logo", req::Permit::any_hash("settings")}, rq.ctx.resource());
  models::accounts::Changes changes;
  if (const req::Param* name = params.get("name"); name != nullptr) changes.name = name->to_s();
  if (const req::Param* settings = params.get("settings"); settings != nullptr) {
    if (const req::ParamMap* map = settings->as_hash()) {
      std::vector<std::pair<std::string, std::string>> items;
      items.reserve(map->size());
      for (std::size_t i = 0; i < map->size(); ++i) {
        items.emplace_back(std::string(map->key_at(i)), map->value_at(i).to_s().value_or(""));
      }
      changes.settings = std::move(items);
    }
  }
  active_storage::Assignment logo = active_storage::assignment_from(params, "logo");
  if (auto staged = co_await active_storage::stage(rq, logo); !staged)
    co_return std::unexpected(std::move(staged.error()));
  active_storage::Applied applied;
  const auto record = models::attachments::Record::account(account_id);
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Status {
    applied = {};
    if (auto updated = models::accounts::update(tx, account_id, changes); !updated) return updated;
    return active_storage::apply(tx, record, "logo", logo, applied);
  });
  if (!written) co_return fail_internal(written.error().message);
  co_await active_storage::after_write(rq, record, logo, applied);
  co_return redirect_to_path(rq, campfire::routes::edit_account(), std::string("\xE2\x9C\x93"));
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::accounts {

Task<net::Response> edit(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::accounts_edit);
}
Task<net::Response> update(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::accounts_update);
}

}  // namespace campfire::routes::accounts
