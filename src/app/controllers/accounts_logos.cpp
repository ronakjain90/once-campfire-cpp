// Accounts::LogosController. Rails: app/controllers/accounts/logos_controller.rb. Rust: crates/campfire/src/
// controllers/accounts/logos.rs.
#include "app/active_storage.hpp"
#include "app/concerns.hpp"
#include "app/controllers/accounts_common.hpp"
#include "app/dispatch.hpp"
#include "assets/assets.hpp"
#include "core/time_format.hpp"
#include "models/account.hpp"
#include "models/attachments.hpp"
#include "routes/routes.hpp"

namespace campfire::app::controllers {

namespace {

// `expires_in 5.minutes, public: true, stale_while_revalidate: 1.week`
constexpr std::uint64_t kMaxAge = 5 * 60;
constexpr std::uint64_t kStaleWhileRevalidate = 7 * 24 * 60 * 60;

// `allow_unauthenticated_access only: :show`
Task<Flow<net::Response>> logos_show(Rq& rq) {
  rq.live_response = true;  // `include ActiveStorage::Streaming`
  auto before = co_await concerns::before_actions(rq, concerns::Before{}.allow_unauthenticated_access());
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto account = models::accounts::first(rq.db(), rq.arena());
  if (!account) co_return fail_internal(account.error().message);

  // `stale?(etag: Current.account)`: there is no accounts/logos/show template to digest.
  Freshness freshness;
  if (*account) {
    const auto updated = parse_db((*account)->updated_at);
    freshness.etag =
        "accounts/" + std::to_string((*account)->id) + "-" + (updated ? format_cache_version(*updated) : "");
  }
  if (auto not_modified = rq.fresh_when(freshness)) co_return std::move(*not_modified);
  expires_in(rq, kMaxAge, true, kStaleWhileRevalidate);

  const bool small = rq.param_str("size") == std::optional<std::string_view>("small");
  if (*account) {
    // `logo.variant(size).processed if logo.variable?`: :small is 192, :large 512, both PNG.
    const std::int64_t size = small ? 192 : 512;
    auto variant =
        co_await active_storage::processed_variant(rq, models::attachments::Record::account((*account)->id), "logo",
                                                   storage::Variation::resize_to_limit(size, size, "png"));
    if (!variant) co_return std::unexpected(std::move(variant.error()));
    if (*variant) {
      const std::string path = rq.app.storage->path_for(**variant).string();
      co_return rq.send_file(path, "image/png", "inline", (**variant).key);
    }
  }
  // `send_stock_icon`
  const std::string_view logical = small ? "logos/app-icon-192.png" : "logos/app-icon.png";
  const auto digested = assets::asset_path(logical);
  const auto file = digested ? assets::find_file(*digested) : std::nullopt;
  if (!file) co_return fail_internal("missing asset " + std::string(logical));
  co_return rq.send_data(file->identity, "image/png", "inline", logical.substr(logical.rfind('/') + 1));
}

// `Current.account.logo.destroy`
Task<Flow<net::Response>> logos_destroy(Rq& rq) {
  rq.live_response = true;  // `include ActiveStorage::Streaming`
  if (auto admin = co_await administrate(rq); !admin) co_return std::unexpected(std::move(admin.error()));
  auto account = models::accounts::first(rq.db(), rq.arena());
  if (!account) co_return fail_internal(account.error().message);
  if (!*account) co_return fail_internal("undefined method 'logo' for nil");
  const auto record = models::attachments::Record::account((*account)->id);
  active_storage::Applied applied;
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Status {
    applied.purge.clear();
    auto removed = models::attachments::destroy(tx, record, "logo", applied.purge);
    if (!removed) return std::unexpected(removed.error());
    return {};
  });
  if (!written) co_return fail_internal(written.error().message);
  active_storage::Assignment none;
  co_await active_storage::after_write(rq, record, none, applied);
  co_return redirect_to_path(rq, campfire::routes::edit_account());
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::accounts_logos {

Task<net::Response> show(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::logos_show);
}
Task<net::Response> destroy(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::logos_destroy);
}

}  // namespace campfire::routes::accounts_logos
