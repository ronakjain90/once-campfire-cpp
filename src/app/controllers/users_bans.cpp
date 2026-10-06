// Users::BansController. Rails: app/controllers/users/bans_controller.rb. Rust: crates/campfire/src/controllers/users/
// bans.rs.
#include "app/concerns.hpp"
#include "app/controllers/accounts_common.hpp"
#include "app/dispatch.hpp"
#include "models/user_admin.hpp"
#include "routes/routes.hpp"

namespace campfire::app::controllers {

namespace {

enum class Ban : std::uint8_t { Create, Destroy };

// `before_action :ensure_can_administer, :set_user`; `@user.ban` or `@user.unban`; `redirect_to @user`.
Task<Flow<net::Response>> change_ban(Rq& rq, Ban action) {
  if (auto admin = co_await administrate(rq); !admin) co_return std::unexpected(std::move(admin.error()));
  // `User.find(params[:user_id])`
  const auto id = id_param(rq, "user_id");
  if (!id) co_return fail_with(ErrorKind::NotFound, "Couldn't find User");
  auto found = models::users::find_by_id(rq.db(), rq.arena(), *id);
  if (!found) co_return fail_internal(found.error().message);
  if (!*found) co_return fail_with(ErrorKind::NotFound, "Couldn't find User");
  const std::int64_t user_id = (*found)->id;
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Status {
    return action == Ban::Create ? models::users::ban(tx, user_id) : models::users::unban(tx, user_id);
  });
  if (!written) co_return fail_internal(written.error().message);
  co_return redirect_to_path(rq, campfire::routes::user(user_id));
}

Task<Flow<net::Response>> bans_create(Rq& rq) {
  return change_ban(rq, Ban::Create);
}
Task<Flow<net::Response>> bans_destroy(Rq& rq) {
  return change_ban(rq, Ban::Destroy);
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::users_bans {

Task<net::Response> create(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::bans_create);
}
Task<net::Response> destroy(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::bans_destroy);
}

}  // namespace campfire::routes::users_bans
