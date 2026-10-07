// Users::PushSubscriptions::TestNotificationsController. Rails: app/controllers/users/push_subscriptions/
// test_notifications_controller.rb. Rust:
// crates/campfire/src/controllers/users/push_subscriptions/test_notifications.rs.
//
// The notification goes out in the request, as in Rails: an error of the push service gives a 500.
#include "app/concerns.hpp"
#include "app/controllers/accounts_common.hpp"
#include "app/dispatch.hpp"
#include "app/web_push.hpp"
#include "models/push_subscription.hpp"
#include "routes/routes.hpp"

namespace campfire::app::controllers {

namespace {

// `Current.user.push_subscriptions.find(params[:push_subscription_id])`
Task<Flow<net::Response>> test_notifications_create(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  const std::int64_t user_id = rq.current_user()->id;
  const auto id = id_param(rq, "push_subscription_id");
  if (!id) co_return fail_with(ErrorKind::NotFound, "Couldn't find Push::Subscription");
  auto found = models::push_subscriptions::find_for_user(rq.db(), rq.arena(), user_id, *id);
  if (!found) co_return fail_internal(found.error().message);
  if (!*found) co_return fail_with(ErrorKind::NotFound, "Couldn't find Push::Subscription");
  auto badge = models::push_subscriptions::unread_count(rq.db(), rq.arena(), user_id);
  if (!badge) co_return fail_internal(badge.error().message);
  const std::string location = rq.url_for(campfire::routes::user_push_subscriptions());
  // Off the worker: the delivery waits for the push service.
  const models::PushSubscription subscription = **found;
  const std::int64_t unread = *badge;
  const auto sent = co_await rq.ctx.offload(
      rq.app.jobs, [&] { return web_push::deliver_test_notification(rq.app, subscription, unread, location); });
  if (sent) co_return fail_internal(*sent);
  co_return rq.redirect_to(location);
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::users_test_notifications {

Task<net::Response> create(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::test_notifications_create);
}

}  // namespace campfire::routes::users_test_notifications
