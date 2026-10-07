// The HTTP server: workers and listeners. Rust: crates/kit/src/front.rs (serve, serve_upstream).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/error.hpp"
#include "net/options.hpp"
#include "net/worker.hpp"

namespace campfire::net {

// Starts one worker for each CPU. Each worker has its own SO_REUSEPORT listeners for HTTP_PORT
// and for TARGET_PORT.
class Server {
 public:
  Server(ServerOptions options, App app);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Binds the listeners and starts the threads. If it fails, no thread runs.
  [[nodiscard]] Status start();
  // Stops the workers: the listeners close, the connections with a request in flight finish
  // (at most 5 seconds), the other connections close. Blocks until the threads end.
  void stop();

  // The ports that the server uses (useful when an option is 0). Valid after `start`.
  [[nodiscard]] std::uint16_t http_port() const noexcept { return http_port_; }
  [[nodiscard]] std::uint16_t https_port() const noexcept { return https_port_; }
  [[nodiscard]] std::uint16_t target_port() const noexcept { return target_port_; }
  [[nodiscard]] std::size_t worker_count() const noexcept { return workers_.size(); }
  [[nodiscard]] std::size_t connection_count() const noexcept;
  // Wakes one worker: it runs `ServerOptions::on_wake` on its own thread. Any thread may call it. A number that is
  // not a worker is ignored.
  void wake(unsigned worker) noexcept;

 private:
  ServerOptions options_;
  App app_;
  std::vector<std::unique_ptr<Worker>> workers_;
  std::uint16_t http_port_ = 0;
  std::uint16_t https_port_ = 0;
  std::uint16_t target_port_ = 0;
};

// The number of CPUs in the cpuset of the process (at least 1).
[[nodiscard]] std::size_t cpuset_size() noexcept;

}  // namespace campfire::net
