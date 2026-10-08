// The Action Cable server of the process. Rust: crates/cable/src/server.rs.
#include "app/channels/server.hpp"

#include <chrono>

#include "cable/protocol.hpp"

namespace campfire::app::channels {

namespace {

std::atomic<CableServer*> g_server{nullptr};

std::int64_t steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
std::int64_t unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

CableServer* cable_server() noexcept {
  return g_server.load(std::memory_order_acquire);
}

CableServer::CableServer(App& app, unsigned workers, std::function<void(unsigned)> wake, CableConfig config)
    : app_(app),
      config_(std::move(config)),
      hub_(workers, wake),
      registry_(make_registry(app, runner_)),
      wake_(std::move(wake)) {
  for (unsigned i = 0; i < workers; ++i) restart_requested_.push_back(std::make_unique<std::atomic<bool>>(false));
  last_drain_ms_.assign(workers, 0);
  app_.hub.store(&hub_, std::memory_order_release);
  g_server.store(this, std::memory_order_release);
}

CableServer::~CableServer() {
  runner_.drain();
  g_server.store(nullptr, std::memory_order_release);
  app_.hub.store(nullptr, std::memory_order_release);
}

void CableServer::on_wake(unsigned worker) {
  if (worker < restart_requested_.size() && restart_requested_[worker]->exchange(false)) {
    hub_.restart(worker);  // never deferred
  } else if (config_.coalesce_ms != 0 && config_.defer && worker < last_drain_ms_.size()) {
    const std::int64_t since = steady_ms() - last_drain_ms_[worker];
    if (since < static_cast<std::int64_t>(config_.coalesce_ms)) {
      // Broadcasts come quickly: let more of them queue, so that each socket writes several frames at once. The call
      // comes again when the window ends; `defer` does nothing if such a call is already due. A deferred call that
      // comes a little early defers again, so the queue is always drained.
      config_.defer(worker, static_cast<std::uint64_t>(static_cast<std::int64_t>(config_.coalesce_ms) - since));
      return;
    }
  }
  hub_.drain(worker);
  if (worker < last_drain_ms_.size()) last_drain_ms_[worker] = steady_ms();
}

void CableServer::on_beat(unsigned worker) {
  hub_.beat(worker, unix_now());
}

std::size_t CableServer::disconnect_user(std::int64_t user_id, bool reconnect) {
  return hub_.broadcast_encoded(cable::protocol::internal_channel(connection_identifier(user_id)),
                                cable::protocol::remote_disconnect_payload(reconnect));
}

void CableServer::restart() {
  for (unsigned i = 0; i < restart_requested_.size(); ++i) {
    restart_requested_[i]->store(true);
    wake_(i);
  }
}

}  // namespace campfire::app::channels
