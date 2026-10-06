// The calls that a model makes to other areas after a commit: the sockets (A7), the jobs (A9). The default does
// nothing. The area that owns the call sets it once at boot. Rails: ActionCable remote_connections, ActiveJob.
// Rust: crates/db/src/events.rs (Event::DisconnectUser, RemoveBannedContent).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace campfire::models::hooks {

using DisconnectUser = std::function<void(std::int64_t user_id, bool reconnect)>;
using RemoveBannedContent = std::function<void(std::int64_t user_id)>;

// `ActionCable.server.remote_connections.where(current_user: user).disconnect(reconnect:)`
void set_disconnect_user(DisconnectUser fn);
void disconnect_user(std::int64_t user_id, bool reconnect);
// `RemoveBannedContentJob.perform_later(user)`
void set_remove_banned_content(RemoveBannedContent fn);
void remove_banned_content(std::int64_t user_id);

// `@push_subscription.notification(title: "Campfire Test", body: Random.uuid, path: user_push_subscriptions_url).deliver`:
// the job area (A9) sends it. `path` is the absolute URL, `badge` is `user.memberships.unread.count`.
using EnqueueTestNotification = std::function<void(std::int64_t subscription_id, std::string path, std::int64_t badge)>;
void set_enqueue_test_notification(EnqueueTestNotification fn);
void enqueue_test_notification(std::int64_t subscription_id, std::string path, std::int64_t badge);

}  // namespace campfire::models::hooks
