// Regression tests of the outbound HTTP client with raw replies that the fuzz target found.
// Rails: Net::HTTP raises "wrong status line" for these.
#include <arpa/inet.h>
#include <doctest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <thread>

#include "app/unfurl_http.hpp"

namespace campfire::app {

namespace {

// Sends `reply` to the first client that connects, then closes the connection.
Result<unfurl::Response> exchange_with_reply(const std::string& reply) {
  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
  REQUIRE(::listen(listener, 1) == 0);
  socklen_t size = sizeof address;
  ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size);
  const std::uint16_t port = ntohs(address.sin_port);
  std::thread server([&] {
    const int fd = ::accept(listener, nullptr, nullptr);
    char request[1024];
    (void)::recv(fd, request, sizeof request, 0);
    (void)::send(fd, reply.data(), reply.size(), MSG_NOSIGNAL);
    ::close(fd);
  });
  unfurl::Network network;
  network.dial_override = [port](std::string&, std::uint16_t& target) { target = port; };
  unfurl::Endpoint endpoint{false, "raw.example", 80, "127.0.0.1"};
  auto response = unfurl::exchange(network, endpoint, unfurl::Request{"GET", "/", "raw.example"},
                                   unfurl::Timeouts{std::chrono::milliseconds(500), std::chrono::milliseconds(500)},
                                   unfurl::Clock::now() + std::chrono::seconds(2));
  server.join();
  ::close(listener);
  return response;
}

}  // namespace

TEST_CASE("unfurl client: a status line with no space is a wrong status line") {
  const auto response = exchange_with_reply("HTTP/33;333333333333333333333e3333\r\n\r\n");
  REQUIRE_FALSE(response.has_value());
  CHECK(response.error().message == "wrong status line");
}

TEST_CASE("unfurl client: a normal status line still parses") {
  const auto response = exchange_with_reply("HTTP/1.1 204 No Content\r\nX-A: b\r\n\r\n");
  REQUIRE(response.has_value());
  CHECK(response->status == 204);
  CHECK(response->reason == "No Content");
}

TEST_CASE("unfurl client: a chunk size line with no end stops at a limit") {
  auto response =
      exchange_with_reply("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + std::string(100000, 'a'));
  REQUIRE(response.has_value());
  const auto body = response->read_body(1 << 20);
  REQUIRE_FALSE(body.has_value());
  CHECK(body.error().message == "wrong chunk size line");
}

}  // namespace campfire::app
