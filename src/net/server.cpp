// The HTTP server: workers and listeners. Rust: crates/kit/src/front.rs, front/conn.rs (bind).
#include "net/server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>

namespace campfire::net {

namespace {

constexpr int kBacklog = 1024;

// Closes a descriptor when the owner goes out of scope.
class Fd {
 public:
  Fd() = default;
  explicit Fd(int fd) noexcept : fd_(fd) {}
  Fd(Fd&& other) noexcept : fd_(other.release()) {}
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = other.release();
    }
    return *this;
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  ~Fd() { reset(); }
  [[nodiscard]] int get() const noexcept { return fd_; }
  int release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }
  void reset() noexcept {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }

 private:
  int fd_ = -1;
};

// A listening socket with SO_REUSEPORT. `address` is an IPv4 or IPv6 literal. The empty text
// means every address, IPv6 and IPv4 ("[::]", then 0.0.0.0, as Go does).
Result<Fd> listen_on(const std::string& address, std::uint16_t port) {
  sockaddr_storage storage{};
  socklen_t length = 0;
  int family = AF_INET6;
  auto fill_v6 = [&](const in6_addr& a) {
    auto* in6 = reinterpret_cast<sockaddr_in6*>(&storage);
    in6->sin6_family = AF_INET6;
    in6->sin6_addr = a;
    in6->sin6_port = htons(port);
    length = sizeof(sockaddr_in6);
    family = AF_INET6;
  };
  auto fill_v4 = [&](const in_addr& a) {
    auto* in4 = reinterpret_cast<sockaddr_in*>(&storage);
    in4->sin_family = AF_INET;
    in4->sin_addr = a;
    in4->sin_port = htons(port);
    length = sizeof(sockaddr_in);
    family = AF_INET;
  };
  bool dual = false;
  if (address.empty()) {
    fill_v6(in6addr_any);
    dual = true;
  } else {
    in_addr a4{};
    in6_addr a6{};
    if (inet_pton(AF_INET, address.c_str(), &a4) == 1) {
      fill_v4(a4);
    } else if (inet_pton(AF_INET6, address.c_str(), &a6) == 1) {
      fill_v6(a6);
    } else {
      return fail(Errc::Config, "TARGET_BIND is not an IP address: " + address);
    }
  }
  for (int attempt = 0; attempt < 2; ++attempt) {
    Fd fd(::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (fd.get() < 0) {
      if (dual && attempt == 0) {
        fill_v4(in_addr{htonl(INADDR_ANY)});
        continue;
      }
      return fail(Errc::Io, std::string("socket: ") + std::strerror(errno));
    }
    const int one = 1;
    const int zero = 0;
    setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd.get(), SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    if (family == AF_INET6) setsockopt(fd.get(), IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&storage), length) != 0 || ::listen(fd.get(), kBacklog) != 0) {
      const int error = errno;
      if (dual && attempt == 0) {
        fill_v4(in_addr{htonl(INADDR_ANY)});
        continue;
      }
      return fail(Errc::Io, "cannot listen on port " + std::to_string(port) + ": " + std::strerror(error));
    }
    return fd;
  }
  return fail(Errc::Io, "cannot listen");
}

std::uint16_t bound_port(int fd) noexcept {
  sockaddr_storage storage{};
  socklen_t length = sizeof storage;
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&storage), &length) != 0) return 0;
  if (storage.ss_family == AF_INET6) return ntohs(reinterpret_cast<sockaddr_in6*>(&storage)->sin6_port);
  return ntohs(reinterpret_cast<sockaddr_in*>(&storage)->sin_port);
}

}  // namespace

std::size_t cpuset_size() noexcept {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof set, &set) == 0) {
    const int count = CPU_COUNT(&set);
    if (count > 0) return static_cast<std::size_t>(count);
  }
  return 1;
}

Server::Server(ServerOptions options, App app) : options_(std::move(options)), app_(app) {
  if (options_.front_headers && !options_.front) options_.front = std::make_shared<front::Front>(FrontConfig{});
  if (!options_.front_headers) options_.front.reset();
  // TLS_DOMAIN (or a TlsServer that a test makes) means the HTTPS port listens (Rust: front.rs).
  if (options_.tls) options_.listen_https = true;
}

Server::~Server() {
  stop();
}

Status Server::start() {
  if (!workers_.empty()) return fail(Errc::Internal, "the server is already started");
  // A write to a socket that the peer closed must not end the process (Rust: Tokio ignores
  // SIGPIPE; `main` does the same for the app). TLS writes cannot pass MSG_NOSIGNAL.
  std::signal(SIGPIPE, SIG_IGN);
  const std::size_t count = options_.workers != 0 ? options_.workers : cpuset_size();
  std::vector<std::unique_ptr<Worker>> workers;
  std::uint16_t http_port = options_.http_port;
  std::uint16_t https_port = options_.https_port;
  std::uint16_t target_port = options_.target_port;
  for (std::size_t i = 0; i < count; ++i) {
    Fd http;
    Fd target;
    Fd https;
    if (options_.listen_http) {
      auto fd = listen_on("", http_port);
      if (!fd) return std::unexpected(fd.error());
      http = std::move(*fd);
      if (i == 0) http_port = bound_port(http.get());
    }
    if (options_.listen_target) {
      auto fd = listen_on(options_.target_bind, target_port);
      if (!fd) return std::unexpected(fd.error());
      target = std::move(*fd);
      if (i == 0) target_port = bound_port(target.get());
    }
    if (options_.listen_https) {
      auto fd = listen_on("", https_port);
      if (!fd) return std::unexpected(fd.error());
      https = std::move(*fd);
      if (i == 0) https_port = bound_port(https.get());
    }
    workers.push_back(std::make_unique<Worker>(options_, app_, http.release(), target.release(), https.release()));
  }
  http_port_ = options_.listen_http ? http_port : 0;
  https_port_ = options_.listen_https ? https_port : 0;
  target_port_ = options_.listen_target ? target_port : 0;
  workers_ = std::move(workers);
  for (const std::unique_ptr<Worker>& worker : workers_) worker->start();
  return {};
}

void Server::stop() {
  for (const std::unique_ptr<Worker>& worker : workers_) worker->stop();
  for (const std::unique_ptr<Worker>& worker : workers_) worker->join();
  workers_.clear();
}

std::size_t Server::connection_count() const noexcept {
  std::size_t total = 0;
  for (const std::unique_ptr<Worker>& worker : workers_) total += worker->connection_count();
  return total;
}

}  // namespace campfire::net
