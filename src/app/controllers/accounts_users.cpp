// Accounts::UsersController. Rails: app/controllers/accounts/users_controller.rb. Rust: crates/campfire/src/
// controllers/accounts/users.rs.
#include "app/concerns.hpp"
#include "app/controllers/accounts_common.hpp"
#include "app/controllers/accounts_users_list.hpp"
#include "app/dispatch.hpp"
#include "compat/ruby.hpp"
#include "models/user_admin.hpp"
#include "routes/routes.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::controllers {

namespace {

// `set_page_and_extract_portion_from User.active.ordered.without_bots, per_page: 500`, rendered as
// `index.turbo_stream.erb` (the only template, so other formats are 406).
Task<Flow<net::Response>> users_index(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  const req::Format offered[] = {&req::mime::TURBO_STREAM};
  if (auto format = rq.respond_to(offered); !format) co_return std::unexpected(std::move(format.error()));
  auto users = models::users::account_users(rq.db(), rq.arena(), false);
  if (!users) co_return fail_internal(users.error().message);
  const Page page = Page::from(rq.param_str("page"), static_cast<std::int64_t>(users->size()));
  views::AccountUsersStreamView view;
  const std::int64_t offset = page.offset();
  for (std::int64_t i = offset; i < offset + Page::kPerPage && i < static_cast<std::int64_t>(users->size()); ++i) {
    view.users.push_back(account_user(rq, (*users)[static_cast<std::size_t>(i)]));
  }
  if (!page.is_last()) view.next_page = std::to_string(page.next_param());
  auto layout = load_layout(rq);
  if (!layout) co_return std::unexpected(std::move(layout.error()));
  const views::ViewContext ctx = make_view_context(rq, *layout);
  Out out(rq.ctx.resource());
  views::accounts::users::index_turbo_stream(out, ctx, view);
  co_return rq.turbo_stream(std::move(out));
}

// `User.active.find(params[:user_id] || params[:id])`
Flow<std::int64_t> set_user(Rq& rq) {
  const auto raw = rq.param_str("user_id") ? rq.param_str("user_id") : rq.param_str("id");
  const auto id = raw ? compat::integer_cast(*raw) : std::nullopt;
  if (id) {
    auto found = models::users::find_active(rq.db(), rq.arena(), *id);
    if (!found) return fail_internal(found.error().message);
    if (*found) return (*found)->id;
  }
  return fail_with(ErrorKind::NotFound, "Couldn't find User");
}

// `@user.update(role: params.require(:user)[:role].presence_in(%w[ member administrator ]) || "member")`
Task<Flow<net::Response>> users_update(Rq& rq) {
  if (auto admin = co_await administrate(rq); !admin) co_return std::unexpected(std::move(admin.error()));
  auto user_id = set_user(rq);
  if (!user_id) co_return std::unexpected(std::move(user_id.error()));
  auto required = rq.params().require("user");
  if (!required) co_return fail_with(ErrorKind::ParameterMissing, required.error().message);
  const req::Param* role = (*required)->get("role");
  const bool administrator = role != nullptr && role->as_str() == std::optional<std::string_view>("administrator");
  models::UserChanges changes;
  changes.role = static_cast<std::int64_t>(administrator ? models::Role::Administrator : models::Role::Member);
  const std::int64_t id = *user_id;
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Status {
    auto updated = models::users::update(tx, id, changes);
    if (!updated) return std::unexpected(updated.error());
    return {};
  });
  if (!written) co_return fail_internal(written.error().message);
  co_return redirect_to_path(rq, campfire::routes::edit_account());
}

// `@user.deactivate`
Task<Flow<net::Response>> users_destroy(Rq& rq) {
  if (auto admin = co_await administrate(rq); !admin) co_return std::unexpected(std::move(admin.error()));
  auto user_id = set_user(rq);
  if (!user_id) co_return std::unexpected(std::move(user_id.error()));
  const std::int64_t id = *user_id;
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(),
                                           [&](db::Tx& tx) -> Status { return models::users::deactivate(tx, id); });
  if (!written) co_return fail_internal(written.error().message);
  co_return redirect_to_path(rq, campfire::routes::edit_account());
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::accounts_users {

Task<net::Response> index(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::users_index);
}
Task<net::Response> update(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::users_update);
}
Task<net::Response> destroy(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::users_destroy);
}

}  // namespace campfire::routes::accounts_users
