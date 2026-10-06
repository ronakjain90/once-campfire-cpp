// Rails: ActionCable remote_connections, ActiveJob. Rust: crates/db/src/events.rs.
#include "models/hooks.hpp"

#include <mutex>
#include <utility>

namespace campfire::models::hooks {

namespace {

struct State {
  std::mutex mutex;
  DisconnectUser disconnect;
  RemoveBannedContent remove;
  EnqueueTestNotification test_notification;
};

State& state() {
  static State s;
  return s;
}

}  // namespace

void set_disconnect_user(DisconnectUser fn) {
  const std::scoped_lock lock(state().mutex);
  state().disconnect = std::move(fn);
}

void disconnect_user(std::int64_t user_id, bool reconnect) {
  DisconnectUser fn;
  {
    const std::scoped_lock lock(state().mutex);
    fn = state().disconnect;
  }
  if (fn) fn(user_id, reconnect);
}

void set_remove_banned_content(RemoveBannedContent fn) {
  const std::scoped_lock lock(state().mutex);
  state().remove = std::move(fn);
}

void remove_banned_content(std::int64_t user_id) {
  RemoveBannedContent fn;
  {
    const std::scoped_lock lock(state().mutex);
    fn = state().remove;
  }
  if (fn) fn(user_id);
}

void set_enqueue_test_notification(EnqueueTestNotification fn) {
  const std::scoped_lock lock(state().mutex);
  state().test_notification = std::move(fn);
}

void enqueue_test_notification(std::int64_t subscription_id, std::string path, std::int64_t badge) {
  EnqueueTestNotification fn;
  {
    const std::scoped_lock lock(state().mutex);
    fn = state().test_notification;
  }
  if (fn) fn(subscription_id, std::move(path), badge);
}

}  // namespace campfire::models::hooks
