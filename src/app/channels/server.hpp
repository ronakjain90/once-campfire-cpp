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
  cable::Hub hub_;
  cable::ChannelRegistry registry_;
  std::function<void(unsigned)> wake_;
  std::vector<std::unique_ptr<std::atomic<bool>>> restart_requested_;
};

// The server of the process, or null before `CableServer` exists.
[[nodiscard]] CableServer* cable_server() noexcept;

}  // namespace campfire::app::channels
