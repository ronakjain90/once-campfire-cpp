// The WebSocket hand-off of the HTTP server: the 101 reply, the bytes in both directions, the queue limits and the
// close. Rails: ActionCable::Connection::ClientSocket. Rust: crates/cable/src/server.rs.
#include <arpa/inet.h>
#include <doctest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "net/server.hpp"

using namespace campfire;
using namespace campfire::net;
using namespace std::chrono_literals;

namespace {

// What the sessions of the test report.
struct Seen {
  std::atomic<int> opened{0};
  std::atomic<int> closed{0};
  std::atomic<int> lagged{0};
  std::atomic<int> stalled{0};
  std::atomic<unsigned> worker{99};
};
Seen seen;

// An echo: each byte string that arrives is sent back. "close" closes with no grace, "bye" with a grace, and
// "flood" queues more buffers than the limit.
class Echo final : public WsSession {
 public:
  explicit Echo(WsTransport& transport) : transport_(transport) {}
  void on_data(std::string_view bytes) override {
    if (bytes == "close") {
      transport_.close(std::chrono::milliseconds(5000));
      return;
    }
    if (bytes == "flood") {
      // One shared buffer: the socket takes a few megabytes, the rest waits in the queue.
      const WsBytes one = std::make_shared<const std::string>(std::string(256 * 1024, 'x'));
      std::vector<WsBytes> many(kWsMaxQueuedBuffers + 300, one);
      transport_.send(many);
      return;
    }
    transport_.send(
        std::vector<WsBytes>{std::make_shared<const std::string>(std::string("echo:") + std::string(bytes))});
  }
  void on_write_stall() override { seen.stalled.fetch_add(1); }
  void on_lagged() override {
    seen.lagged.fetch_add(1);
    transport_.close(std::chrono::milliseconds(0));
  }
  void on_closed() override { seen.closed.fetch_add(1); }

 private:
  WsTransport& transport_;
};

Task<Response> handler(Ctx& ctx) {
  Response response = ctx.response(101);
  response.add("upgrade", "websocket");
  response.add("connection", "upgrade");
  response.ws_accept = [](WsTransport& transport, unsigned worker) -> std::unique_ptr<WsSession> {
    seen.opened.fetch_add(1);
    seen.worker.store(worker);
    transport.send(std::vector<WsBytes>{std::make_shared<const std::string>("welcome")});
    return std::make_unique<Echo>(transport);
  };
  co_return response;
}

class Socket {
 public:
  explicit Socket(std::uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    REQUIRE(::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    timeval tv{3, 0};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  }
  ~Socket() { ::close(fd_); }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  void send(std::string_view text) const {
    REQUIRE(::send(fd_, text.data(), text.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(text.size()));
  }
  // Reads until `text` is in the buffer (or the socket ends). Returns the whole buffer.
  std::string read_until(std::string_view text) {
    while (buffer_.find(text) == std::string::npos) {
      char chunk[4096];
      const ssize_t n = ::recv(fd_, chunk, sizeof chunk, 0);
      if (n <= 0) break;
      buffer_.append(chunk, static_cast<std::size_t>(n));
    }
    return buffer_;
  }
  // True if the peer closed the connection.
  bool at_end() {
    char chunk[4096];
    while (true) {
      const ssize_t n = ::recv(fd_, chunk, sizeof chunk, 0);
      if (n == 0) return true;
      if (n < 0) return false;
      buffer_.append(chunk, static_cast<std::size_t>(n));
    }
  }
  void shutdown_write() const { ::shutdown(fd_, SHUT_WR); }
  [[nodiscard]] const std::string& buffer() const { return buffer_; }

 private:
  int fd_ = -1;
  std::string buffer_;
};

struct Fixture {
  Fixture() {
    ServerOptions options;
    options.http_port = 0;
    options.target_port = 0;
    options.workers = 2;
    options.front_headers = false;
    server = std::make_unique<Server>(options, App{nullptr, &handler});
    REQUIRE(server->start().has_value());
    seen.opened = seen.closed = seen.lagged = seen.stalled = 0;
  }
  std::unique_ptr<Server> server;
};

constexpr const char* kUpgrade = "GET /cable HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n";

bool wait_for(const std::atomic<int>& value, int expected) {
  for (int i = 0; i < 200; ++i) {
    if (value.load() == expected) return true;
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

}  // namespace

TEST_CASE("websocket: the 101 reply, then the bytes of the session in both directions") {
  Fixture f;
  Socket s(f.server->target_port());
  s.send(kUpgrade);
  const std::string text = s.read_until("welcome");
  CHECK(
      text.starts_with("HTTP/1.1 101 Switching Protocols\r\nupgrade: websocket\r\nconnection: upgrade\r\n\r\nwelcome"));
  CHECK(seen.opened.load() == 1);
  CHECK(seen.worker.load() < 2);
  s.send("hello");
  CHECK(s.read_until("echo:hello").ends_with("echo:hello"));
  s.send("again");
  CHECK(s.read_until("echo:again").ends_with("echo:again"));
}

TEST_CASE("websocket: bytes that come with the request reach the session") {
  Fixture f;
  Socket s(f.server->target_port());
  s.send(std::string(kUpgrade) + "early");
  CHECK(s.read_until("echo:early").ends_with("echo:early"));
}

TEST_CASE("websocket: the end of the peer runs on_closed once") {
  Fixture f;
  {
    Socket s(f.server->target_port());
    s.send(kUpgrade);
    s.read_until("welcome");
  }
  CHECK(wait_for(seen.closed, 1));
  std::this_thread::sleep_for(100ms);
  CHECK(seen.closed.load() == 1);
}

TEST_CASE("websocket: close writes the queue, shuts the write side and waits for the peer") {
  Fixture f;
  Socket s(f.server->target_port());
  s.send(kUpgrade);
  s.read_until("welcome");
  s.send("close");
  CHECK(s.at_end());   // the server shut its write side
  s.shutdown_write();  // the peer answers: the server closes at once
  CHECK(wait_for(seen.closed, 1));
}

TEST_CASE("websocket: more queued buffers than the limit is a lag") {
  Fixture f;
  Socket s(f.server->target_port());
  s.send(kUpgrade);
  s.read_until("welcome");
  s.send("flood");
  CHECK(wait_for(seen.lagged, 1));
  CHECK(wait_for(seen.closed, 1));
}

TEST_CASE("websocket: stopping the server closes the sessions") {
  auto f = std::make_unique<Fixture>();
  Socket s(f->server->target_port());
  s.send(kUpgrade);
  s.read_until("welcome");
  f->server->stop();
  CHECK(seen.closed.load() == 1);
  CHECK(s.at_end());
}
