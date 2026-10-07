// libFuzzer target of the whole server: HTTP/1.1 and HTTP/2 (prior knowledge) over a loopback
// socket. It runs the worker, the parser, the chunked decoder, nghttp2 and the front pipeline.
// Rust: crates/kit/src/front/conn.rs.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "net/server.hpp"

namespace {

using namespace campfire::net;

campfire::Task<Response> echo_handler(Ctx& ctx) {
  const Request& request = ctx.request();
  Response response = ctx.response(200);
  std::string text(request.method_text);
  text += ' ';
  text += request.path;
  text += ' ';
  text += request.query;
  for (const Header& h : request.headers) {
    text += h.name;
    text += h.value;
  }
  text += request.body;
  response.add("content-type", "text/plain");
  response.add_copy("content-length", std::to_string(text.size()));
  response.body_view(ctx.arena().copy(text));
  co_return response;
}

struct Harness {
  Harness() {
    ServerOptions options;
    options.http_port = 0;
    options.target_port = 0;
    options.https_port = 0;
    options.listen_target = false;
    options.h2c = true;
    options.workers = 1;
    options.serve_static = false;
    options.idle_timeout_ms = 2000;
    options.read_timeout_ms = 2000;
    options.max_request_body = 1 << 20;
    server = std::make_unique<Server>(options, App{nullptr, &echo_handler});
    if (!server->start().has_value()) std::abort();
  }
  std::unique_ptr<Server> server;
};

// The client preface and an empty SETTINGS frame: the input starts as an HTTP/2 connection.
constexpr char kSettings[] = {0, 0, 0, 4, 0, 0, 0, 0, 0};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  static Harness harness;
  if (size == 0) return 0;
  std::string bytes;
  if (data[0] & 1) {
    bytes.assign(kH2Preface);
    bytes.append(kSettings, sizeof kSettings);
  }
  bytes.append(reinterpret_cast<const char*>(data) + 1, size - 1);

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(harness.server->http_port());
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  timeval tv{0, 4'000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
      const ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
      if (n <= 0) break;
      sent += static_cast<std::size_t>(n);
    }
    ::shutdown(fd, SHUT_WR);
    char buffer[4096];
    std::size_t total = 0;
    while (total < (1 << 16)) {
      const ssize_t n = ::recv(fd, buffer, sizeof buffer, 0);
      if (n <= 0) break;
      total += static_cast<std::size_t>(n);
    }
  }
  ::close(fd);
  return 0;
}
