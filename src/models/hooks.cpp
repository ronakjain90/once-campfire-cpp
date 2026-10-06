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
  PurgeBlob purge;
  AnalyzeBlob analyze;
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

void set_purge_blob(PurgeBlob fn) {
  const std::scoped_lock lock(state().mutex);
  state().purge = std::move(fn);
}

void purge_blob(std::int64_t blob_id) {
  PurgeBlob fn;
  {
    const std::scoped_lock lock(state().mutex);
    fn = state().purge;
  }
  if (fn) fn(blob_id);
}

void set_analyze_blob(AnalyzeBlob fn) {
  const std::scoped_lock lock(state().mutex);
  state().analyze = std::move(fn);
}

void analyze_blob(std::int64_t blob_id) {
  AnalyzeBlob fn;
  {
    const std::scoped_lock lock(state().mutex);
    fn = state().analyze;
  }
  if (fn) fn(blob_id);
}

}  // namespace campfire::models::hooks
