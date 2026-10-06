// In-process server tests. Rust: crates/kit/tests/front.rs and http.rs.
#include <doctest.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "net/server.hpp"

using namespace campfire;
using namespace campfire::net;
using namespace std::chrono_literals;

namespace {

ThreadPool& pool() {
  static ThreadPool instance(2);
  return instance;
}

constexpr std::size_t kBigSize = 6 << 20;

// One handler for all tests; the path chooses the behavior.
Task<Response> test_handler(Ctx& ctx) {
  const Request& request = ctx.request();
  Response response = ctx.response(200);
  response.add("content-type", "text/plain");
  if (request.path == "/big") {
    char* data = static_cast<char*>(ctx.arena().allocate(kBigSize, 1));
    for (std::size_t i = 0; i < kBigSize; ++i) data[i] = static_cast<char>('a' + i % 26);
    response.body_view({data, kBigSize});
  } else if (request.path == "/slow") {
    const int value = co_await ctx.offload(pool(), [] {
      std::this_thread::sleep_for(50ms);
      return 7;
    });
    response.body_view(ctx.arena().copy("slow " + std::to_string(value)));
  } else if (request.path == "/throw") {
    throw std::runtime_error("boom");
  } else {
    // Echo: "<method> <path>?<query> <body>".
    std::string text = std::string(request.method_text) + " " + std::string(request.path);
    if (!request.query.empty()) text += "?" + std::string(request.query);
    text += " " + std::string(request.body);
    response.body_view(ctx.arena().copy(text));
  }
  co_return response;
}

struct Reply {
  int status = 0;
  std::string head;
  std::string body;
  bool closed = false;  // the server closed the connection
};

class Client {
 public:
  explicit Client(std::uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    REQUIRE(::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    timeval tv{5, 0};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  }
  ~Client() { ::close(fd_); }
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  void send(std::string_view text) const {
    while (!text.empty()) {
      const ssize_t n = ::send(fd_, text.data(), text.size(), MSG_NOSIGNAL);
      REQUIRE(n > 0);
      text.remove_prefix(static_cast<std::size_t>(n));
    }
  }
  [[nodiscard]] int fd() const { return fd_; }

  // Reads one response. `slow_ms` pauses between reads (to fill the buffers of the server).
  Reply read_reply(bool head_request = false, int slow_ms = 0) {
    Reply reply;
    while (true) {
      const std::size_t end = buffer_.find("\r\n\r\n");
      if (end != std::string::npos) {
        reply.head = buffer_.substr(0, end);
        buffer_.erase(0, end + 4);
        break;
      }
      if (!fill(0)) {
        reply.closed = true;
        return reply;
      }
    }
    reply.status = std::stoi(reply.head.substr(9, 3));
    std::string lower = reply.head;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (head_request || reply.status == 204 || reply.status == 304) return reply;
    if (const std::size_t p = lower.find("content-length: "); p != std::string::npos) {
      const std::size_t n = std::stoul(lower.substr(p + 16));
      while (buffer_.size() < n) {
        if (!fill(slow_ms)) break;
      }
      reply.body = buffer_.substr(0, n);
      buffer_.erase(0, n);
    } else if (lower.find("transfer-encoding: chunked") != std::string::npos) {
      while (true) {
        while (buffer_.find("\r\n") == std::string::npos) {
          if (!fill(slow_ms)) return reply;
        }
        const std::size_t eol = buffer_.find("\r\n");
        const std::size_t n = std::stoul(buffer_.substr(0, eol), nullptr, 16);
        while (buffer_.size() < eol + 2 + n + 2) {
          if (!fill(slow_ms)) return reply;
        }
        reply.body += buffer_.substr(eol + 2, n);
        buffer_.erase(0, eol + 2 + n + 2);
        if (n == 0) break;
      }
    }
    return reply;
  }

 private:
  bool fill(int slow_ms) {
    if (slow_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(slow_ms));
    char chunk[16384];
    const ssize_t n = ::recv(fd_, chunk, sizeof chunk, 0);
    if (n <= 0) return false;
    buffer_.append(chunk, static_cast<std::size_t>(n));
    return true;
  }
  int fd_ = -1;
  std::string buffer_;
};

struct Fixture {
  explicit Fixture(ServerOptions options = {}) {
    options.http_port = 0;
    options.target_port = 0;
    options.workers = 2;
    server = std::make_unique<Server>(options, App{nullptr, &test_handler});
    REQUIRE(server->start().has_value());
  }
  [[nodiscard]] std::uint16_t http() const { return server->http_port(); }
  [[nodiscard]] std::uint16_t target() const { return server->target_port(); }
  std::unique_ptr<Server> server;
};

constexpr const char* kGet = "GET /a?b=1 HTTP/1.1\r\nHost: x\r\n\r\n";

}  // namespace

TEST_CASE("server keeps the connection alive and serves both ports") {
  Fixture f;
  for (const std::uint16_t port : {f.http(), f.target()}) {
    Client c(port);
    for (int i = 0; i < 3; ++i) {
      c.send(kGet);
      const Reply r = c.read_reply();
      CHECK(r.status == 200);
      CHECK(r.body == "GET /a?b=1 ");
    }
  }
}

TEST_CASE("front headers only on the front port") {
  Fixture f;
  Client front(f.http());
  front.send(kGet);
  const Reply a = front.read_reply();
  CHECK(a.head.find("x-cache: miss") != std::string::npos);
  CHECK(a.head.find("date: ") != std::string::npos);
  Client target(f.target());
  target.send(kGet);
  const Reply b = target.read_reply();
  CHECK(b.head.find("x-cache") == std::string::npos);
}

TEST_CASE("server answers pipelined requests in order") {
  Fixture f;
  Client c(f.http());
  std::string all;
  for (int i = 0; i < 5; ++i) all += "POST /p" + std::to_string(i) + " HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nb" + std::to_string(i);
  c.send(all);
  for (int i = 0; i < 5; ++i) {
    const Reply r = c.read_reply();
    CHECK(r.body == "POST /p" + std::to_string(i) + " b" + std::to_string(i));
  }
}

TEST_CASE("server reads chunked bodies sent in pieces, and 100-continue") {
  Fixture f;
  Client c(f.http());
  c.send("POST /c HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel");
  std::this_thread::sleep_for(50ms);
  c.send("lo\r\n6\r\n world\r\n0\r\n\r\n");
  CHECK(c.read_reply().body == "POST /c hello world");
  c.send("POST /e HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nExpect: 100-continue\r\n\r\n");
  const Reply cont = c.read_reply();
  CHECK(cont.status == 100);
  c.send("xyz");
  CHECK(c.read_reply().body == "POST /e xyz");
}

TEST_CASE("server answers HEAD without a body, and HTTP/1.0 closes") {
  Fixture f;
  Client c(f.http());
  c.send("HEAD /h HTTP/1.1\r\nHost: x\r\n\r\n");
  const Reply h = c.read_reply(true);
  CHECK(h.status == 200);
  CHECK(h.head.find("content-length: 8") != std::string::npos);
  c.send("GET /z HTTP/1.0\r\nHost: x\r\n\r\n");
  const Reply old = c.read_reply();
  CHECK(old.head.rfind("HTTP/1.0 200", 0) == 0);
  CHECK(old.head.find("connection: close") != std::string::npos);
  CHECK(c.read_reply().closed);
}

TEST_CASE("server rejects bad requests and a handler that throws gives 500") {
  Fixture f;
  Client bad(f.http());
  bad.send("BAD\r\n\r\n");
  CHECK(bad.read_reply().status == 400);
  Client t(f.http());
  t.send("GET /throw HTTP/1.1\r\nHost: x\r\n\r\n");
  CHECK(t.read_reply().status == 500);
  t.send(kGet);
  CHECK(t.read_reply().status == 200);  // the connection still works
}

TEST_CASE("server sends 413 above MAX_REQUEST_BODY") {
  ServerOptions options;
  options.max_request_body = 10;
  Fixture f(options);
  Client a(f.http());
  a.send("POST /u HTTP/1.1\r\nHost: x\r\nContent-Length: 11\r\n\r\nhello world");
  const Reply r = a.read_reply();
  CHECK(r.status == 413);
  CHECK(r.head.find("x-cache: bypass") != std::string::npos);
  Client b(f.http());
  b.send("POST /u HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n6\r\nhello \r\n5\r\nworld\r\n0\r\n\r\n");
  CHECK(b.read_reply().status == 413);
  Client ok(f.http());
  ok.send("POST /u HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello");
  CHECK(ok.read_reply().body == "POST /u hello");
  Client t(f.target());
  t.send("POST /u HTTP/1.1\r\nHost: x\r\nContent-Length: 11\r\n\r\nhello world");
  const Reply tr = t.read_reply();
  CHECK(tr.status == 413);
  CHECK(tr.head.find("x-cache") == std::string::npos);
}

TEST_CASE("server sends 413 above the memory limit for bodies") {
  ServerOptions options;
  options.max_buffered_body = 8;
  Fixture f(options);
  Client a(f.http());
  a.send("POST /u HTTP/1.1\r\nHost: x\r\nContent-Length: 9\r\n\r\n123456789");
  CHECK(a.read_reply().status == 413);
}

TEST_CASE("server writes a large response to a slow reader") {
  Fixture f;
  Client c(f.http());
  int small = 32768;
  setsockopt(c.fd(), SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
  c.send("GET /big HTTP/1.1\r\nHost: x\r\n\r\n");
  const Reply r = c.read_reply(false, 1);
  REQUIRE(r.body.size() == kBigSize);
  bool same = true;
  for (std::size_t i = 0; i < kBigSize; i += 4093) same = same && r.body[i] == static_cast<char>('a' + i % 26);
  CHECK(same);
  c.send(kGet);  // the connection is still good
  CHECK(c.read_reply().status == 200);
}

TEST_CASE("handler resumes on the worker after offload") {
  Fixture f;
  Client c(f.http());
  c.send("GET /slow HTTP/1.1\r\nHost: x\r\n\r\n");
  CHECK(c.read_reply().body == "slow 7");
  // Many at once, on both workers.
  std::vector<std::unique_ptr<Client>> clients;
  for (int i = 0; i < 16; ++i) {
    clients.push_back(std::make_unique<Client>(f.http()));
    clients.back()->send("GET /slow HTTP/1.1\r\nHost: x\r\n\r\n");
  }
  for (auto& client : clients) CHECK(client->read_reply().body == "slow 7");
}

TEST_CASE("idle connections close at the shorter of the idle and read timeouts") {
  ServerOptions options;
  options.idle_timeout_ms = 3000;
  options.read_timeout_ms = 300;
  Fixture f(options);
  Client c(f.http());
  c.send(kGet);
  CHECK(c.read_reply().status == 200);
  const auto start = std::chrono::steady_clock::now();
  CHECK(c.read_reply().closed);
  const auto waited = std::chrono::steady_clock::now() - start;
  CHECK(waited >= 250ms);
  CHECK(waited < 2000ms);
}

TEST_CASE("a slow request head gets 408") {
  ServerOptions options;
  options.read_timeout_ms = 300;
  Fixture f(options);
  Client c(f.http());
  c.send("GET / HTTP/1.1\r\nHost: x\r\n");
  const Reply r = c.read_reply();
  CHECK((r.closed || r.status == 408));
}

TEST_CASE("a slow request body gets 408") {
  ServerOptions options;
  options.read_timeout_ms = 300;
  options.idle_timeout_ms = 5000;
  Fixture f(options);
  Client c(f.http());
  c.send("POST /u HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc");
  CHECK(c.read_reply().status == 408);
}

TEST_CASE("server stops with connections open") {
  Fixture f;
  Client c(f.http());
  c.send(kGet);
  CHECK(c.read_reply().status == 200);
  f.server->stop();
  CHECK(c.read_reply().closed);
}
