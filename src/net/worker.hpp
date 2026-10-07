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
#include "net/h2.hpp"
#include "net/options.hpp"
#include "net/timer_wheel.hpp"

namespace campfire::net {

// ActionCable::Server::Connections::BEAT_INTERVAL: the heartbeat of the sockets.
inline constexpr std::uint64_t kBeatIntervalMs = 3000;

// One worker owns an epoll loop, its listeners and its connections. It is the `Scheduler` of the
// handlers that run on its connections: a posted coroutine resumes on the thread of the worker.
class Worker final : public Scheduler {
 public:
  // The worker takes the ownership of the listener descriptors (-1 for none).
  Worker(const ServerOptions& options, const App& app, int http_listener, int target_listener, int https_listener = -1,
         unsigned index = 0);
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

  // The index of this worker in the server: the hub of the app keeps one queue for each worker.
  [[nodiscard]] unsigned index() const noexcept { return index_; }
  // Asks the worker to run `ServerOptions::on_wake`. Any thread may call it.
  void request_wake();

  // The number of connections now. For tests. Any thread may call it.
  [[nodiscard]] std::size_t connection_count() const noexcept { return connection_count_.load(); }

 private:
  struct Source;  // the tag in the data of an epoll event

  void run();
  void handle_event(void* tag, std::uint32_t events);
  void on_accept(int listener_fd, bool via_front, bool tls);
  void watch_listeners(bool on);
  void pause_accept();
  void run_posted();
  void drain_finished();
  void sweep();

  // Connection state machine (conn.cpp).
  void pump(Conn& c);
  bool step_head(Conn& c);
  bool step_body(Conn& c);
  bool step_write(Conn& c);
  bool step_linger(Conn& c);
  bool step_handshake(Conn& c);
  bool fill(Conn& c);  // reads from the socket; false if the connection ended or has no data
  // `fill` for a connection whose head is parsed: the request holds views into the read buffer, so if the buffer moves,
  // the head is parsed again from the new place.
  bool fill_body(Conn& c);
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
  // WebSocket (worker_ws.cpp).
  void ws_start(Conn& c, Response& response);
  bool ws_step(Conn& c);
  void ws_flush(Conn& c);
  void ws_notify_closed(Conn& c);
  void ws_on_timer(Conn& c, TimerNode& node);
  // The same two steps for one HTTP/2 stream.
  Task<void> h2_serve(Conn& c, H2Stream& stream);
  // Arms a timer only when its due time is earlier than the one that is armed.
  void arm_earlier(Conn& c, std::uint64_t due_ms, TimerKind kind);

 public:
  // Transport: plain or TLS (worker_io.cpp).
  enum class Io : std::uint8_t { Ok, WouldBlock, Closed };
  // Reads up to `size` bytes. `got` is the number read. Closed: the peer ended, or an error.
  Io io_read(Conn& c, char* buffer, std::size_t size, std::size_t& got);
  // Writes some of `data`. `wrote` is the number written.
  Io io_write(Conn& c, const char* data, std::size_t size, std::size_t& wrote);
  // Writes all of a small text now if the socket takes it (best effort). For 100-continue and errors.
  void send_text(Conn& c, std::string_view text);
  void shutdown_write(Conn& c);

  // The HTTP/2 code (worker_h2.cpp) calls these.
  void h2_start(Conn& c);
  bool h2_step(Conn& c);
  void h2_on_timer(Conn& c);
  void h2_begin_request(Conn& c, H2Stream& stream);
  void h2_handler_finished(Conn& c, H2Stream& stream, Response&& response);
  [[nodiscard]] const ServerOptions& options() const noexcept { return options_; }
  [[nodiscard]] bool stopping() const noexcept { return stopping_.load(); }
  [[nodiscard]] std::uint64_t now_ms_public() const noexcept { return now_ms_; }
  [[nodiscard]] std::unique_ptr<Arena> take_arena_public() { return take_arena(); }
  void give_arena_public(std::unique_ptr<Arena> arena) { give_arena(std::move(arena)); }
  void post_finished(Conn& c) { finished_.push_back(&c); }
  friend struct WsState;

 private:
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
  int https_listener_ = -1;
  std::thread thread_;
  std::atomic<std::thread::id> owner_;
  std::atomic<bool> stopping_{false};
  unsigned index_ = 0;
  std::atomic<bool> wake_pending_{false};
  TimerNode beat_node_;
  std::uint64_t accept_resume_ms_ = 0;  // not 0: the listeners are out of epoll until this time

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
