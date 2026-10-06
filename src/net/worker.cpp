// Worker thread: epoll loop, listeners, accept, posted coroutines. Rust: crates/kit/src/front/conn.rs.
#include "net/worker.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>

#include "core/log.hpp"

namespace campfire::net {

namespace {

constexpr std::uint64_t kTagWake = 1;
constexpr std::uint64_t kTagHttp = 2;
constexpr std::uint64_t kTagTarget = 3;
constexpr std::uint64_t kStopGraceMs = 5000;
constexpr int kMaxEvents = 256;
constexpr int kAcceptBatch = 128;

std::uint64_t monotonic_ms() noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000 + static_cast<std::uint64_t>(ts.tv_nsec) / 1'000'000;
}

void add_to_epoll(int epoll_fd, int fd, std::uint32_t events, std::uint64_t tag) {
  epoll_event event{};
  event.events = events;
  event.data.u64 = tag;
  epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event);
}

}  // namespace

ServerOptions ServerOptions::from_config(const FrontConfig& config) {
  ServerOptions options;
  options.http_port = config.http_port;
  options.target_port = config.target_port;
  options.target_bind = config.target_bind;
  options.idle_timeout_ms = config.http_idle_timeout_s * 1000;
  options.read_timeout_ms = config.http_read_timeout_s * 1000;
  options.write_timeout_ms = config.http_write_timeout_s * 1000;
  options.max_request_body = static_cast<std::uint64_t>(std::max<std::int64_t>(config.max_request_body, 0));
  // The Rust front does not listen twice on one port.
  if (config.target_port == config.http_port) options.listen_target = false;
  return options;
}

Worker::Worker(const ServerOptions& options, const App& app, int http_listener, int target_listener)
    : options_(options),
      app_(app),
      http_listener_(http_listener),
      target_listener_(target_listener),
      now_ms_(monotonic_ms()),
      wheel_(now_ms_) {
  epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
  wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  add_to_epoll(epoll_fd_, wake_fd_, EPOLLIN, kTagWake);
  if (http_listener_ >= 0) add_to_epoll(epoll_fd_, http_listener_, EPOLLIN, kTagHttp);
  if (target_listener_ >= 0) add_to_epoll(epoll_fd_, target_listener_, EPOLLIN, kTagTarget);
}

Worker::~Worker() {
  join();
  for (std::unique_ptr<Conn>& c : conns_) {
    if (c->handler_active) {
      // A coroutine may wait for another thread. Its frame must not be destroyed. Keep it.
      log_warn("worker stopped with a handler that did not end; its memory stays");
      (void)c.release();
    }
  }
  conns_.clear();
  graveyard_.clear();
  for (const int fd : {http_listener_, target_listener_, wake_fd_, epoll_fd_}) {
    if (fd >= 0) ::close(fd);
  }
}

void Worker::post(std::coroutine_handle<> handle) {
  {
    const std::lock_guard lock(post_mutex_);
    posted_.push_back(handle);
  }
  posted_count_.fetch_add(1, std::memory_order_release);
  if (!on_owner_thread()) {
    const std::uint64_t one = 1;
    [[maybe_unused]] const ssize_t n = ::write(wake_fd_, &one, sizeof one);
  }
}

bool Worker::on_owner_thread() const noexcept { return owner_.load() == std::this_thread::get_id(); }

void Worker::start() {
  thread_ = std::thread([this] {
    owner_.store(std::this_thread::get_id());
    run();
  });
}

void Worker::stop() {
  stopping_.store(true);
  const std::uint64_t one = 1;
  [[maybe_unused]] const ssize_t n = ::write(wake_fd_, &one, sizeof one);
}

void Worker::join() {
  if (thread_.joinable()) thread_.join();
}

void Worker::run() {
  epoll_event events[kMaxEvents];
  bool stop_begun = false;
  std::uint64_t stop_deadline = 0;
  while (true) {
    now_ms_ = monotonic_ms();
    wheel_.advance(now_ms_, [this](TimerNode& node) { on_timer(node); });
    if (stopping_.load() && !stop_begun) {
      stop_begun = true;
      stop_deadline = now_ms_ + kStopGraceMs;
      for (const int fd : {http_listener_, target_listener_}) {
        if (fd >= 0) epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
      }
      // Idle connections close now. The others close when their response is written.
      for (std::size_t i = conns_.size(); i-- > 0;) {
        Conn& c = *conns_[i];
        if (!c.closed && ((c.state == ConnState::Head && c.rbuf.empty()) || c.state == ConnState::Linger)) {
          close_conn(c);
        }
      }
    }
    if (stop_begun && (conns_.empty() || now_ms_ >= stop_deadline)) break;
    int timeout = posted_count_.load(std::memory_order_acquire) != 0 ? 0 : wheel_.next_wait_ms(now_ms_);
    if (stop_begun && (timeout < 0 || timeout > 50)) timeout = 50;
    const int n = epoll_wait(epoll_fd_, events, kMaxEvents, timeout);
    now_ms_ = monotonic_ms();
    for (int i = 0; i < n; ++i) {
      handle_event(events[i].data.ptr, events[i].events);
    }
    run_posted();
    drain_finished();
    sweep();
  }
  for (std::size_t i = conns_.size(); i-- > 0;) {
    if (!conns_[i]->closed) close_conn(*conns_[i]);
  }
  sweep();
}

void Worker::handle_event(void* tag, std::uint32_t events) {
  const auto value = reinterpret_cast<std::uintptr_t>(tag);
  if (value == kTagWake) {
    std::uint64_t counter = 0;
    [[maybe_unused]] const ssize_t n = ::read(wake_fd_, &counter, sizeof counter);
    return;
  }
  if (value == kTagHttp || value == kTagTarget) {
    if (!stopping_.load()) on_accept(value == kTagHttp ? http_listener_ : target_listener_, value == kTagHttp);
    return;
  }
  Conn& c = *static_cast<Conn*>(tag);
  if (c.closed) return;
  if ((events & (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP)) != 0) c.readable = true;
  if ((events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0) c.writable = true;
  pump(c);
}

void Worker::on_accept(int listener_fd, bool via_front) {
  for (int i = 0; i < kAcceptBatch; ++i) {
    sockaddr_storage address{};
    socklen_t length = sizeof address;
    const int fd = accept4(listener_fd, reinterpret_cast<sockaddr*>(&address), &length, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR || errno == ECONNABORTED) continue;
      return;  // EAGAIN, or a limit of descriptors: try at the next event
    }
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    auto conn = std::make_unique<Conn>();
    Conn& c = *conn;
    c.fd = fd;
    c.via_front = via_front;
    c.timer.owner = &c;
    // The text of the peer address in canonical form: an IPv4-mapped IPv6 address is IPv4.
    const char* text = nullptr;
    if (address.ss_family == AF_INET) {
      text = inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&address)->sin_addr, c.remote, sizeof c.remote);
    } else if (address.ss_family == AF_INET6) {
      const in6_addr& a6 = reinterpret_cast<sockaddr_in6*>(&address)->sin6_addr;
      text = IN6_IS_ADDR_V4MAPPED(&a6) ? inet_ntop(AF_INET, &a6.s6_addr[12], c.remote, sizeof c.remote)
                                         : inet_ntop(AF_INET6, &a6, c.remote, sizeof c.remote);
    }
    c.remote_size = text != nullptr ? static_cast<std::uint8_t>(std::strlen(c.remote)) : 0;
    c.index = conns_.size();
    conns_.push_back(std::move(conn));
    connection_count_.fetch_add(1);
    epoll_event event{};
    event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
    event.data.ptr = &c;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event);
    begin_wait(c);
  }
}

void Worker::run_posted() {
  while (posted_count_.load(std::memory_order_acquire) != 0) {
    {
      const std::lock_guard lock(post_mutex_);
      posted_run_.swap(posted_);
      posted_count_.store(0, std::memory_order_release);
    }
    for (const std::coroutine_handle<> handle : posted_run_) {
      handle.resume();
      drain_finished();
    }
    posted_run_.clear();
  }
}

void Worker::drain_finished() {
  if (draining_) return;
  draining_ = true;
  for (std::size_t i = 0; i < finished_.size(); ++i) {
    Conn* c = finished_[i];
    c->task = Task<void>{};  // the frame is at its last suspension point
    if (c->closed) {
      close_conn(*c);  // the descriptor is closed already: this removes the object
    } else {
      pump(*c);
    }
  }
  finished_.clear();
  draining_ = false;
}

void Worker::sweep() { graveyard_.clear(); }

std::unique_ptr<Arena> Worker::take_arena() {
  if (!arenas_.empty()) {
    std::unique_ptr<Arena> arena = std::move(arenas_.back());
    arenas_.pop_back();
    return arena;
  }
  return std::make_unique<Arena>();
}

void Worker::give_arena(std::unique_ptr<Arena> arena) {
  arena->reset();
  if (arenas_.size() < 1024) arenas_.push_back(std::move(arena));
}

}  // namespace campfire::net
