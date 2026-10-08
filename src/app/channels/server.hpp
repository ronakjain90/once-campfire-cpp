// The Action Cable server of the process: the hub, the channels, the origin rules, the heartbeat and the remote
// disconnects. Rails: ActionCable::Server::Base. Rust: crates/cable/src/server.rs.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/app.hpp"
#include "app/channels/channels.hpp"
#include "app/channels/detached.hpp"
#include "cable/hub.hpp"

namespace campfire::app::channels {

// `config.action_cable.*` as the production reference runs it.
struct CableConfig {
  bool disable_request_forgery_protection = false;
  // Exact `Origin` values accepted in addition to the same-origin rule.
  std::vector<std::string> allowed_request_origins;
  bool allow_same_origin_as_host = true;
  // `config.assume_ssl` (on unless DISABLE_SSL): the same-origin check compares with `https://<host>`.
  bool assume_ssl = true;
  // Under load, a worker drains its broadcast queue at most once in this many milliseconds, so that each socket gets
  // the frames of several broadcasts in one write (CAMPFIRE_CABLE_COALESCE_MS). 0: drain at each wake. A worker that
  // did not drain within the window drains at once, so the delay is only added when broadcasts come quickly. With
  // 1,000 clients in one room, 4 ms gave about 12% more broadcasts a second than draining at each wake (and 8% more
  // than the Rust port), with no change of the median delivery time.
  unsigned coalesce_ms = 4;
  // `defer(worker, ms)`: call `on_wake` of that worker again after `ms`. The worker's own thread calls it. Needed when
  // `coalesce_ms` is not 0.
  std::function<void(unsigned, std::uint64_t)> defer;
};

class CableServer {
 public:
  // Makes the hub for `workers` workers and gives it to the app (`App::hub`). `wake(worker)` tells that worker to call
  // `drain`. The server must be the only one of the process.
  CableServer(App& app, unsigned workers, std::function<void(unsigned)> wake, CableConfig config = {});
  ~CableServer();
  CableServer(const CableServer&) = delete;
  CableServer& operator=(const CableServer&) = delete;

  [[nodiscard]] App& app() noexcept { return app_; }
  [[nodiscard]] cable::Hub& hub() noexcept { return hub_; }
  [[nodiscard]] const cable::ChannelRegistry& registry() const noexcept { return registry_; }
  [[nodiscard]] const CableConfig& config() const noexcept { return config_; }

  // The worker thread of `worker` calls these (from `ServerOptions::on_wake` and `on_beat`).
  void on_wake(unsigned worker);
  void on_beat(unsigned worker);

  // `ActionCable.server.remote_connections.where(current_user: user).disconnect(reconnect:)`. Returns the number of
  // sockets that listen.
  std::size_t disconnect_user(std::int64_t user_id, bool reconnect);
  // `ActionCable.server.restart`: every socket closes with `server_restart`. Any thread may call it.
  void restart();

 private:
  App& app_;
  CableConfig config_;
  DetachedRunner runner_;  // declared first: it ends last
  cable::Hub hub_;
  cable::ChannelRegistry registry_;
  std::function<void(unsigned)> wake_;
  std::vector<std::unique_ptr<std::atomic<bool>>> restart_requested_;
  // By worker, used only on that worker's thread: the end of its last drain.
  std::vector<std::int64_t> last_drain_ms_;
};

// The server of the process, or null before `CableServer` exists.
[[nodiscard]] CableServer* cable_server() noexcept;

}  // namespace campfire::app::channels
