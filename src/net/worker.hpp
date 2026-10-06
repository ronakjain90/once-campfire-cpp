// Worker thread: epoll loop, listeners, connections, timers. Rust: crates/kit/src/front/conn.rs.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/arena.hpp"
#include "core/scheduler.hpp"
#include "net/conn.hpp"
#include "net/options.hpp"
#include "net/timer_wheel.hpp"

namespace campfire::net {

// One worker owns an epoll loop, its listeners and its connections. It is the `Scheduler` of the
// handlers that run on its connections: a posted coroutine resumes on the thread of the worker.
class Worker final : public Scheduler {
 public:
  // The worker takes the ownership of the listener descriptors (-1 for none).
  Worker(const ServerOptions& options, const App& app, int http_listener, int target_listener);
  ~Worker() override;
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  // Scheduler.
  void post(std::coroutine_handle<> handle) override;
  [[nodiscard]] bool on_owner_thread() const noexcept override;

  // Starts the thread. `stop` ends the loop. `join` waits for the end.
  void start();
  void stop();
  void join();

  // The number of connections now. For tests. Any thread may call it.
  [[nodiscard]] std::size_t connection_count() const noexcept { return connection_count_.load(); }

 private:
  struct Source;  // the tag in the data of an epoll event

  void run();
  void handle_event(void* tag, std::uint32_t events);
  void on_accept(int listener_fd, bool via_front);
  void run_posted();
  void drain_finished();
  void sweep();

  // Connection state machine (conn.cpp).
  void pump(Conn& c);
  bool step_head(Conn& c);
  bool step_body(Conn& c);
  bool step_write(Conn& c);
  bool step_linger(Conn& c);
  bool fill(Conn& c);  // reads from the socket; false if the connection ended or has no data
  void begin_wait(Conn& c);
  void dispatch(Conn& c);
  void begin_write(Conn& c);
  void request_done(Conn& c);
  void reply_error(Conn& c, int status);
  void send_continue(Conn& c);
  void on_timer(TimerNode& node);
  void close_conn(Conn& c);
  void abort_conn(Conn& c);
  Task<void> serve(Conn& c);
  void handler_finished(Conn& c, Response&& response);

  std::unique_ptr<Arena> take_arena();
  void give_arena(std::unique_ptr<Arena> arena);
  void discard(Conn& c);
  void release_request(Conn& c);
  [[nodiscard]] std::uint64_t now_ms() const noexcept { return now_ms_; }
  void arm(Conn& c, std::int64_t period_ms, std::uint64_t since_ms, TimerKind kind);

  ServerOptions options_;
  App app_;
  int epoll_fd_ = -1;
  int wake_fd_ = -1;
  int http_listener_ = -1;
  int target_listener_ = -1;
  std::thread thread_;
  std::atomic<std::thread::id> owner_;
  std::atomic<bool> stopping_{false};

  std::mutex post_mutex_;
  std::vector<std::coroutine_handle<>> posted_;
  std::vector<std::coroutine_handle<>> posted_run_;
  std::atomic<std::size_t> posted_count_{0};

  std::uint64_t now_ms_ = 0;
  TimerWheel wheel_;
  std::vector<std::unique_ptr<Conn>> conns_;
  std::vector<Conn*> finished_;
  std::vector<std::unique_ptr<Conn>> graveyard_;
  bool draining_ = false;
  std::vector<std::unique_ptr<Arena>> arenas_;
  std::atomic<std::size_t> connection_count_{0};
};

}  // namespace campfire::net
