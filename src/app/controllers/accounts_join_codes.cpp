// Accounts::JoinCodesController. Rails: app/controllers/accounts/join_codes_controller.rb. Rust: crates/campfire/src/
// controllers/accounts/join_codes.rs.
#include "app/concerns.hpp"
#include "app/controllers/accounts_common.hpp"
#include "app/dispatch.hpp"
#include "models/account_admin.hpp"
#include "routes/routes.hpp"

namespace campfire::app::controllers {

namespace {

// `Current.account.reset_join_code`
Task<Flow<net::Response>> join_codes_create(Rq& rq) {
  if (auto admin = co_await administrate(rq); !admin) co_return std::unexpected(std::move(admin.error()));
  auto account = models::accounts::first_edit(rq.db(), rq.arena());
  if (!account) co_return fail_internal(account.error().message);
  if (!*account) co_return fail_internal("undefined method 'reset_join_code' for nil");
  const std::int64_t id = (*account)->id;
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(),
                                           [&](db::Tx& tx) -> Status { return models::accounts::reset_join_code(tx, id); });
  if (!written) co_return fail_internal(written.error().message);
  co_return redirect_to_path(rq, campfire::routes::edit_account());
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::accounts_join_codes {

Task<net::Response> create(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::join_codes_create);
}

}  // namespace campfire::routes::accounts_join_codes
