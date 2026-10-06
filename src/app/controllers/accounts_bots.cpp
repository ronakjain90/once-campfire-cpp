// Accounts::BotsController. Rails: app/controllers/accounts/bots_controller.rb. Rust: crates/campfire/src/controllers/
// accounts/bots.rs.
#include "app/active_storage.hpp"
#include "app/concerns.hpp"
#include "app/controllers/accounts_common.hpp"
#include "app/controllers/sidebars.hpp"
#include "app/dispatch.hpp"
#include "models/attachments.hpp"
#include "models/user_admin.hpp"
#include "routes/routes.hpp"
#include "storage/paths.hpp"
#include "views/accounts/types.hpp"
#include "views/helpers/forms.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::controllers {

namespace {

// `User.active_bots.find(id)`: 404 when there is no such bot.
Flow<models::User> find_active_bot(Rq& rq, std::string_view key) {
  const auto id = id_param(rq, key);
  if (id) {
    auto found = models::users::find_active_bot(rq.db(), rq.arena(), *id);
    if (!found) return fail_internal(found.error().message);
    if (*found) return std::move(**found);
  }
  return fail_with(ErrorKind::NotFound, "Couldn't find User");
}

// `params.require(:user).permit(:name, :avatar, :webhook_url)`
Flow<req::ParamMap> bot_params(Rq& rq) {
  auto required = rq.params().require("user");
  if (!required) return fail_with(ErrorKind::ParameterMissing, required.error().message);
  const req::ParamMap* hash = (*required)->as_hash();
  if (hash == nullptr) return req::ParamMap(rq.ctx.resource());
  return hash->permit({"name", "avatar", "webhook_url"}, rq.ctx.resource());
}

std::optional<std::string> text_of(const req::ParamMap& params, std::string_view key) {
  const req::Param* value = params.get(key);
  return value == nullptr ? std::nullopt : value->to_s();
}

Flow<net::Response> redirect_to_bots(Rq& rq) {
  return redirect_to_path(rq, campfire::routes::account_bots());
}

Task<Flow<net::Response>> bots_index(Rq& rq) {
  if (auto admin = co_await administrate(rq); !admin) co_return std::unexpected(std::move(admin.error()));
  const req::Format offered[] = {&req::mime::HTML};
  if (auto format = rq.respond_to(offered); !format) co_return std::unexpected(std::move(format.error()));
  db::DependencyScope& deps = rq.track();
  auto layout = load_layout(rq);
  if (!layout) co_return std::unexpected(std::move(layout.error()));
  auto bots = models::users::active_bots_ordered(rq.db(), rq.arena());
  if (!bots) co_return fail_internal(bots.error().message);
  std::vector<views::BotView> views;
  for (const models::User& bot : *bots) {
    views::BotView view;
    view.user = user_summary(rq, bot);
    view.bot_key = models::users::bot_key(bot);
    auto rooms = models::users::bot_rooms(rq.db(), rq.arena(), bot.id);
    if (!rooms) co_return fail_internal(rooms.error().message);
    for (const models::users::BotRoom& room : *rooms) view.rooms.push_back({room.id, room.name});
    views.push_back(std::move(view));
  }
  PageSpec spec;
  spec.name = "accounts/bots#index";
  spec.title = "Chat bots";
  spec.nav = [](Out& out, const views::ViewContext& ctx) { views::accounts::bots::index_nav(out, ctx); };
  spec.content = [&](Out& out, const views::ViewContext& ctx) { views::accounts::bots::index(out, ctx, views); };
  co_return render_tracked_page(rq, 200, deps, *layout, spec);
}

Task<Flow<net::Response>> bots_new(Rq& rq) {
  if (auto admin = co_await administrate(rq); !admin) co_return std::unexpected(std::move(admin.error()));
  const req::Format offered[] = {&req::mime::HTML};
  if (auto format = rq.respond_to(offered); !format) co_return std::unexpected(std::move(format.error()));
  db::DependencyScope& deps = rq.track();
  auto layout = load_layout(rq);
  if (!layout) co_return std::unexpected(std::move(layout.error()));
  // `form_with model: @bot, url: account_bots_path, class: "flex flex-column gap"`
  const views::helpers::FormWith form =
      views::helpers::form_with_url(campfire::routes::account_bots()).model("user").cls("flex flex-column gap");
  const views::BotFormView bot;
  PageSpec spec;
  spec.name = "accounts/bots#new";
  spec.title = "New chat bot";
  spec.nav = [](Out& out, const views::ViewContext& ctx) { views::accounts::bots::back_nav(out, ctx); };
  spec.content = [&](Out& out, const views::ViewContext& ctx) { views::accounts::bots::new_(out, ctx, bot, form); };
  co_return render_tracked_page(rq, 200, deps, *layout, spec);
}

// `User.create_bot! bot_params`
Task<Flow<net::Response>> bots_create(Rq& rq) {
  if (auto admin = co_await administrate(rq); !admin) co_return std::unexpected(std::move(admin.error()));
  auto params = bot_params(rq);
  if (!params) co_return std::unexpected(std::move(params.error()));
  // users.name is NOT NULL.
  const auto name = text_of(*params, "name");
  if (!name) co_return fail_internal("NOT NULL constraint failed: users.name");
  // `create_webhook!(url: webhook_url) if webhook_url`: any value that is not nil, "" too.
  const auto webhook_url = text_of(*params, "webhook_url");
  active_storage::Assignment avatar = active_storage::assignment_from(*params, "avatar");
  if (auto staged = co_await active_storage::stage(rq, avatar); !staged)
    co_return std::unexpected(std::move(staged.error()));
  active_storage::Applied applied;
  std::int64_t bot_id = 0;
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Status {
    applied = {};
    auto bot = models::users::create_bot(tx, *name, webhook_url);
    if (!bot) return std::unexpected(bot.error());
    bot_id = bot->id;
    return active_storage::apply(tx, models::attachments::Record::user(bot->id), "avatar", avatar, applied);
  });
  if (!written) co_return fail_internal(written.error().message);
  co_await active_storage::after_write(rq, models::attachments::Record::user(bot_id), avatar, applied);
  co_return redirect_to_bots(rq);
}

Task<Flow<net::Response>> bots_edit(Rq& rq) {
  if (auto admin = co_await administrate(rq); !admin) co_return std::unexpected(std::move(admin.error()));
  db::DependencyScope& deps = rq.track();
  auto layout = load_layout(rq);
  if (!layout) co_return std::unexpected(std::move(layout.error()));
  auto bot = find_active_bot(rq, "id");
  if (!bot) co_return std::unexpected(std::move(bot.error()));
  const req::Format offered[] = {&req::mime::HTML};
  if (auto format = rq.respond_to(offered); !format) co_return std::unexpected(std::move(format.error()));
  views::BotFormView view;
  view.name = bot->name;
  auto webhook = models::users::webhook_url(rq.db(), rq.arena(), bot->id);
  if (!webhook) co_return fail_internal(webhook.error().message);
  view.webhook_url = *webhook;
  // `image_tag bot.avatar`: the absolute URL of the blob redirect.
  models::attachments::AttachmentRecords records(rq.db());
  auto attached = records.attached("User", bot->id, "avatar");
  if (!attached) co_return fail_internal(attached.error().message);
  if (*attached) {
    view.avatar_attachment_url =
        rq.info.base_url() + storage::paths::blob_redirect_path(rq.app.storage->verifier(), **attached);
  }
  const std::int64_t bot_id = bot->id;
  const views::helpers::FormWith form = views::helpers::form_with_url(campfire::routes::account_bot(bot_id))
                                            .model("user")
                                            .method("patch")
                                            .cls("flex flex-column gap");
  PageSpec spec;
  spec.name = "accounts/bots#edit";
  spec.title = "Edit bot";
  spec.nav = [](Out& out, const views::ViewContext& ctx) { views::accounts::bots::back_nav(out, ctx); };
  spec.content = [&](Out& out, const views::ViewContext& ctx) {
    views::accounts::bots::edit(out, ctx, bot_id, view, form);
  };
  spec.facets = [&](db::DependencyScope& d, const LayoutData&) { d.facet("bot", static_cast<std::uint64_t>(bot_id)); };
  co_return render_tracked_page(rq, 200, deps, *layout, spec);
}

// `@bot.update_bot! bot_params`: the webhook first, then the bot, in one transaction.
Task<Flow<net::Response>> bots_update(Rq& rq) {
  if (auto admin = co_await administrate(rq); !admin) co_return std::unexpected(std::move(admin.error()));
  auto bot = find_active_bot(rq, "id");
  if (!bot) co_return std::unexpected(std::move(bot.error()));
  auto params = bot_params(rq);
  if (!params) co_return std::unexpected(std::move(params.error()));
  models::UserChanges changes;
  changes.name = text_of(*params, "name");
  const auto webhook_url = text_of(*params, "webhook_url");
  active_storage::Assignment avatar = active_storage::assignment_from(*params, "avatar");
  if (auto staged = co_await active_storage::stage(rq, avatar); !staged)
    co_return std::unexpected(std::move(staged.error()));
  active_storage::Applied applied;
  const std::int64_t bot_id = bot->id;
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Status {
    applied = {};
    if (auto updated = models::users::update_bot(tx, bot_id, changes, webhook_url); !updated) return updated;
    return active_storage::apply(tx, models::attachments::Record::user(bot_id), "avatar", avatar, applied);
  });
  if (!written) co_return fail_internal(written.error().message);
  co_await active_storage::after_write(rq, models::attachments::Record::user(bot_id), avatar, applied);
  co_return redirect_to_bots(rq);
}

// `@bot.deactivate`
Task<Flow<net::Response>> bots_destroy(Rq& rq) {
  if (auto admin = co_await administrate(rq); !admin) co_return std::unexpected(std::move(admin.error()));
  auto bot = find_active_bot(rq, "id");
  if (!bot) co_return std::unexpected(std::move(bot.error()));
  const std::int64_t bot_id = bot->id;
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(),
                                           [&](db::Tx& tx) -> Status { return models::users::deactivate(tx, bot_id); });
  if (!written) co_return fail_internal(written.error().message);
  co_return redirect_to_bots(rq);
}

// Accounts::Bots::KeysController. `User.active_bots.find(params[:bot_id]).reset_bot_key`
Task<Flow<net::Response>> bot_keys_update(Rq& rq) {
  if (auto admin = co_await administrate(rq); !admin) co_return std::unexpected(std::move(admin.error()));
  auto bot = find_active_bot(rq, "bot_id");
  if (!bot) co_return std::unexpected(std::move(bot.error()));
  const std::int64_t bot_id = bot->id;
  auto written = co_await rq.app.db->write(
      rq.ctx.scheduler(), [&](db::Tx& tx) -> Status { return models::users::reset_bot_key(tx, bot_id); });
  if (!written) co_return fail_internal(written.error().message);
  co_return redirect_to_bots(rq);
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::accounts_bots {

Task<net::Response> index(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::bots_index);
}
Task<net::Response> new_(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::bots_new);
}
Task<net::Response> create(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::bots_create);
}
Task<net::Response> edit(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::bots_edit);
}
Task<net::Response> update(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::bots_update);
}
Task<net::Response> destroy(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::bots_destroy);
}

}  // namespace campfire::routes::accounts_bots

namespace campfire::routes::accounts_bots_keys {

Task<net::Response> update(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::bot_keys_update);
}

}  // namespace campfire::routes::accounts_bots_keys
