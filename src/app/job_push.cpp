// Room::PushMessageJob and Room::MessagePusher. Rails: app/jobs/room/push_message_job.rb,
// app/models/room/message_pusher.rb, lib/web_push/pool.rb. Rust:
// crates/campfire/src/integrations/{jobs.rs,web_push.rs,web_push/pool.rs}.
#include <algorithm>
#include <mutex>
#include <thread>

#include "app/job_support.hpp"
#include "app/message_presenter.hpp"
#include "app/web_push.hpp"
#include "core/log.hpp"
#include "core/time_format.hpp"
#include "jobs/web_push.hpp"
#include "models/room_ref.hpp"
#include "richtext/richtext.hpp"

namespace campfire::app::job {

namespace {

// `Membership::Connectable::CONNECTION_TTL`
constexpr std::int64_t kConnectionTtlSeconds = 60;

struct Payload {
  std::string title;
  std::string body;
  std::string path;
};

// `Room::MessagePusher#build_payload`, with the limits of the Rust port: a title or a body that is too long is cut
// short, so that the notification fits in one record.
Payload build_payload(MessagePresenter& presenter, const models::RoomRef& room, const models::Message& message) {
  const models::User creator = must(presenter.user(message.creator_id));
  const std::string body = must(presenter.plain_text_body(message));
  Payload payload;
  payload.path = "/rooms/" + std::to_string(room.id);
  if (room.direct()) {
    payload.title = creator.name;
    payload.body = body;
  } else {
    payload.title = room.name.value_or("");
    payload.body = creator.name + ": " + body;
  }
  payload.title = jobs::web_push::truncate_json_string(std::move(payload.title), jobs::web_push::kMaxPayloadTitleBytes);
  payload.body = jobs::web_push::truncate_json_string(std::move(payload.body), jobs::web_push::kMaxPayloadBodyBytes);
  return payload;
}

// `Message#mentionees`: `room.users.where(id: mentioned_users.map(&:id))`.
std::vector<std::int64_t> mentionee_ids(db::Connection& conn, Arena& arena, MessagePresenter& presenter,
                                        const models::RoomRef& room, const models::Message& message) {
  const std::string html = must(presenter.body_html(message));
  if (html.empty()) return {};
  const auto mentioned = richtext::mentioned_users(html, presenter.render_context());
  if (!mentioned) raise(mentioned.error().message);
  std::vector<std::int64_t> ids;
  for (const richtext::MentionUser& user : *mentioned) ids.push_back(user.id);
  std::vector<std::int64_t> out;
  for (const models::User& user : must(models::room_refs::members_among(conn, arena, room.id, ids))) {
    out.push_back(user.id);
  }
  return out;
}

// Delivers each notification, at most `deliveries` at a time, and then destroys the subscriptions that cannot be
// delivered to (`WebPush::Pool`: one worker destroys them in order).
void deliver_all(App& app, std::counting_semaphore<64>& deliveries, std::vector<web_push::Notification> notifications) {
  std::mutex mutex;
  std::vector<std::int64_t> invalid;
  std::vector<std::thread> threads;
  const std::int64_t now = app.now().seconds;
  for (web_push::Notification& notification : notifications) {
    deliveries.acquire();
    threads.emplace_back([&app, &deliveries, &mutex, &invalid, now, notification = std::move(notification)] {
      const auto delivered = web_push::deliver(notification, *app.vapid, app.push_network, now);
      if (!delivered) {
        if (delivered.error().invalidates_subscription()) {
          const std::scoped_lock lock(mutex);
          invalid.push_back(notification.subscription.id);
        } else {
          log_error("Error in WebPush::Pool.deliver: {} {}", delivered.error().class_name, delivered.error().message);
        }
      }
      deliveries.release();
    });
  }
  for (std::thread& thread : threads) thread.join();
  for (const std::int64_t id : invalid) {
    log_info("Destroying push subscription: {}", id);
    const auto destroyed = write(app, [id](db::Tx& tx) { return models::push_subscriptions::destroy(tx, id); });
    if (!destroyed) log_error("Error in WebPush::Pool.invalid_subscription_handler: {}", destroyed.error().message);
  }
}

}  // namespace

void push_message(App& app, std::counting_semaphore<64>& deliveries, std::int64_t room_id, std::int64_t message_id) {
  // `Room::PushMessageJob` does nothing when Web Push is off.
  if (!app.vapid) return;
  JobThread& thread = job_thread();
  db::Connection& conn = thread.reader;
  Arena arena(8192);
  auto message = must(models::messages::find_by_id(conn, arena, message_id));
  if (!message) raise("Couldn't find Message with 'id'=" + std::to_string(message_id));
  auto room = must(models::room_refs::find(conn, arena, room_id));
  if (!room) raise("Couldn't find Room with 'id'=" + std::to_string(room_id));
  MessagePresenter presenter(conn, arena, app, std::string());
  const Payload payload = build_payload(presenter, *room, *message);

  const std::string cutoff = format_db(app.now().plus_seconds(-kConnectionTtlSeconds));
  auto everything =
      must(models::push_subscriptions::involved_in_everything(conn, arena, room->id, message->creator_id, cutoff));
  auto mentions = must(models::push_subscriptions::involved_in_mentions(
      conn, arena, room->id, message->creator_id, mentionee_ids(conn, arena, presenter, *room, *message), cutoff));
  const auto by_id = [](const models::PushSubscription& a, const models::PushSubscription& b) { return a.id < b.id; };
  std::ranges::sort(everything, by_id);
  std::ranges::sort(mentions, by_id);

  // `Push::Subscription#notification`: the badge is `user.memberships.unread.count`, counted here.
  std::vector<web_push::Notification> notifications;
  for (auto* group : {&everything, &mentions}) {
    for (models::PushSubscription& subscription : *group) {
      web_push::Notification notification;
      notification.title = payload.title;
      notification.body = payload.body;
      notification.path = payload.path;
      notification.badge = must(models::push_subscriptions::unread_count(conn, arena, subscription.user_id));
      notification.subscription = std::move(subscription);
      notifications.push_back(std::move(notification));
    }
  }
  deliver_all(app, deliveries, std::move(notifications));
}

}  // namespace campfire::app::job
