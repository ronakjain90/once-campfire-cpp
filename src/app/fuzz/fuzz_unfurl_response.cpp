// libFuzzer target: the response of a server that a user asked to unfurl. The bytes go through the client of the
// unfurl (the head, the framing, the inflate) from a local server. Rust: crates/campfire/src/integrations/net/http.rs.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

#include "app/unfurl_http.hpp"

namespace {

int make_listener(std::uint16_t& port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || ::listen(fd, 16) != 0) std::abort();
  socklen_t size = sizeof address;
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size);
  port = ntohs(address.sin_port);
  return fd;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace campfire::app::unfurl;
  static std::uint16_t port = 0;
  static const int listener = make_listener(port);
  const std::string reply(reinterpret_cast<const char*>(data), size);
  std::thread server([&] {
    const int fd = ::accept(listener, nullptr, nullptr);
    if (fd < 0) return;
    char request[1024];
    (void)::recv(fd, request, sizeof request, 0);
    std::size_t done = 0;
    while (done < reply.size()) {
      const ssize_t n = ::send(fd, reply.data() + done, reply.size() - done, MSG_NOSIGNAL);
      if (n <= 0) break;
      done += static_cast<std::size_t>(n);
    }
    ::close(fd);
  });
  Network network;
  Endpoint endpoint{false, "fuzz.example", 80, "127.0.0.1"};
  network.dial_override = [](std::string&, std::uint16_t& target) { target = port; };
  const Timeouts timeouts{std::chrono::milliseconds(200), std::chrono::milliseconds(200)};
  auto response = exchange(network, endpoint, Request{"GET", "/", "fuzz.example"}, timeouts,
                           Clock::now() + std::chrono::seconds(1));
  if (response) {
    auto body = response->read_body(1 << 20);
    if (body && !body->too_large && body->bytes.size() > (1U << 20)) std::abort();
  }
  server.join();
  return 0;
}
