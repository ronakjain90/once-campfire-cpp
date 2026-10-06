// Users::PushSubscriptionsController. Rails: app/controllers/users/push_subscriptions_controller.rb. Rust: crates/
// campfire/src/controllers/users/push_subscriptions.rs.
#include "app/concerns.hpp"
#include "app/controllers/accounts_common.hpp"
#include "app/dispatch.hpp"
#include "app/network_guard.hpp"
#include "app/user_agent.hpp"
#include "models/push_subscription.hpp"
#include "routes/routes.hpp"
#include "views/accounts/types.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::controllers {

namespace {

views::PushSubscriptionView push_subscription_view(const models::PushSubscription& subscription) {
  // `UserAgent.parse(push_subscription.user_agent)`: a nil browser or platform prints nothing.
  const ua::Agent agent = ua::parse(subscription.user_agent.value_or(""));
  views::PushSubscriptionView view;
  view.id = subscription.id;
  view.endpoint = subscription.endpoint.value_or("");
  view.browser = agent.browser();
  view.version = agent.version().str();
  const auto platform = agent.try_platform();
  view.platform = platform && *platform ? **platform : std::string{};
  return view;
}

Task<Flow<net::Response>> push_subscriptions_index(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  const req::Format offered[] = {&req::mime::HTML};
  if (auto format = rq.respond_to(offered); !format) co_return std::unexpected(std::move(format.error()));
  db::DependencyScope& deps = rq.track();
  auto layout = load_layout(rq);
  if (!layout) co_return std::unexpected(std::move(layout.error()));
  auto subscriptions = models::push_subscriptions::for_user(rq.db(), rq.arena(), rq.current_user()->id);
  if (!subscriptions) co_return fail_internal(subscriptions.error().message);
  std::vector<views::PushSubscriptionView> views;
  for (const models::PushSubscription& subscription : *subscriptions) views.push_back(push_subscription_view(subscription));

  PageSpec spec;
  spec.name = "users/push_subscriptions#index";
  spec.title = "Push notification subscriptions";
  spec.nav = [](Out& out, const views::ViewContext& ctx) { views::users::push_subscriptions::index_nav(out, ctx); };
  spec.content = [&](Out& out, const views::ViewContext& ctx) {
    views::users::push_subscriptions::index(out, ctx, views);
  };
  co_return render_tracked_page(rq, 200, deps, *layout, spec);
}

// `wrap_parameters format: [:json]` for the key `push_subscription`: a JSON body that does not have the key is
// nested under it. The hash holds the permitted keys.
Flow<req::ParamMap> push_subscription_params(Rq& rq) {
  const req::ParamMap& all = rq.params();
  const auto type = req::content_mime_type(rq.request.header("content-type"));
  const bool json = type && *type == &req::mime::JSON;
  const req::ParamMap* source = nullptr;
  req::ParamMap wrapped(rq.ctx.resource());
  if (json && !all.contains("push_subscription")) {
    for (std::size_t i = 0; i < all.size(); ++i) {
      const std::string_view key = all.key_at(i);
      if (key == "authenticity_token" || key == "_method" || key == "utf8") continue;
      wrapped.insert(key, all.value_at(i).clone(rq.ctx.resource()));
    }
    source = &wrapped;
  } else {
    auto required = all.require("push_subscription");
    if (!required) return fail_with(ErrorKind::ParameterMissing, required.error().message);
    source = (*required)->as_hash();
  }
  if (source == nullptr) return req::ParamMap(rq.ctx.resource());
  return source->permit({"endpoint", "p256dh_key", "auth_key"}, rq.ctx.resource());
}

// `RestrictedHTTP::PrivateNetworkGuard.resolve(endpoint_uri.host)`, done on a job thread for the one host that the
// validation asks about.
struct Resolved {
  std::optional<std::string> host;
  std::optional<std::string> address;
};

Task<Resolved> resolve_endpoint(Rq& rq, const models::PushSubscription& subscription) {
  Resolved out;
  out.host = models::push_subscriptions::endpoint_host_to_resolve(subscription.endpoint);
  if (!out.host) co_return out;
  const std::string host = *out.host;
  out.address = co_await rq.ctx.offload(rq.app.jobs, [host] { return resolve_public_address(host); });
  co_return out;
}

models::ResolveHost resolver_of(const Resolved& resolved) {
  return [&resolved](std::string_view host) -> std::optional<std::string> {
    if (resolved.host && *resolved.host == host) return resolved.address;
    return std::nullopt;
  };
}

Task<Flow<net::Response>> push_subscriptions_create(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  const std::int64_t user_id = rq.current_user()->id;
  auto params = push_subscription_params(rq);
  if (!params) co_return std::unexpected(std::move(params.error()));
  const auto text = [&](std::string_view key) -> std::optional<std::string> {
    const req::Param* value = params->get(key);
    return value == nullptr ? std::nullopt : value->to_s();
  };

  // `@push_subscriptions.find_by(push_subscription_params)`
  models::push_subscriptions::Conditions conditions;
  conditions.endpoint_given = params->contains("endpoint");
  conditions.endpoint = text("endpoint");
  conditions.p256dh_key_given = params->contains("p256dh_key");
  conditions.p256dh_key = text("p256dh_key");
  conditions.auth_key_given = params->contains("auth_key");
  conditions.auth_key = text("auth_key");
  auto existing = models::push_subscriptions::find_by(rq.db(), rq.arena(), user_id, conditions);
  if (!existing) co_return fail_internal(existing.error().message);
  if (*existing) {
    // Existing endpoints must pass current validations.
    const Resolved resolved = co_await resolve_endpoint(rq, **existing);
    if (!models::push_subscriptions::validate(**existing, resolver_of(resolved)).empty()) co_return rq.head(422);
    const std::int64_t id = (*existing)->id;
    auto touched = co_await rq.app.db->write(rq.ctx.scheduler(),
                                             [&](db::Tx& tx) -> Status { return models::push_subscriptions::touch(tx, id); });
    if (!touched) co_return fail_internal(touched.error().message);
    co_return rq.head(200);
  }
  models::PushSubscription subscription;
  subscription.user_id = user_id;
  subscription.endpoint = text("endpoint");
  subscription.p256dh_key = text("p256dh_key");
  subscription.auth_key = text("auth_key");
  if (rq.request.has_header("user-agent")) subscription.user_agent = std::string(rq.user_agent());
  const Resolved resolved = co_await resolve_endpoint(rq, subscription);
  const models::ResolveHost resolve = resolver_of(resolved);
  auto created = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Status {
    auto saved = models::push_subscriptions::create(tx, subscription, resolve);
    if (!saved) return std::unexpected(saved.error());
    return {};
  });
  if (!created) {
    // `head subscription.persisted? ? :ok : :unprocessable_entity`
    if (created.error().code == Errc::InvalidArgument) co_return rq.head(422);
    co_return fail_internal(created.error().message);
  }
  co_return rq.head(200);
}

// `@push_subscriptions.destroy_by(id: params[:id])`
Task<Flow<net::Response>> push_subscriptions_destroy(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  const std::int64_t user_id = rq.current_user()->id;
  if (const auto id = id_param(rq, "id")) {
    auto done = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) -> Status {
      return models::push_subscriptions::destroy_by_id(tx, user_id, *id);
    });
    if (!done) co_return fail_internal(done.error().message);
  }
  co_return redirect_to_path(rq, campfire::routes::user_push_subscriptions());
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::users_push_subscriptions {

Task<net::Response> index(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::push_subscriptions_index);
}
Task<net::Response> create(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::push_subscriptions_create);
}
Task<net::Response> destroy(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::push_subscriptions_destroy);
}

}  // namespace campfire::routes::users_push_subscriptions
