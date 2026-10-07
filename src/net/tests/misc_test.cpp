// Tests of net/response.hpp, timer_wheel.hpp, thread_pool.hpp, front.hpp.
#include <doctest.h>

#include <atomic>
#include <string>

#include "core/arena.hpp"
#include "net/front.hpp"
#include "net/front/acme_http.hpp"
#include "net/response.hpp"
#include "net/thread_pool.hpp"
#include "net/timer_wheel.hpp"

using namespace campfire::net;

namespace {
std::string wire_text(const Wire& wire) {
  iovec iov[64];
  const std::size_t n = wire.fill_iovecs(iov, 0);
  std::string out;
  for (std::size_t i = 0; i < n; ++i) out.append(static_cast<const char*>(iov[i].iov_base), iov[i].iov_len);
  CHECK(out.size() == wire.total());
  return out;
}
}  // namespace

TEST_CASE("Wire keeps the header order and adds the length after connection") {
  campfire::Arena arena;
  Response response(arena.resource(), 200);
  response.add("b", "1");
  response.add("a", "2");
  response.body_view("hello");
  WireOptions options;
  options.close = true;
  CHECK(wire_text(Wire(arena.resource(), response, options)) ==
        "HTTP/1.1 200 OK\r\nb: 1\r\na: 2\r\nconnection: close\r\ncontent-length: 5\r\n\r\nhello");
}

TEST_CASE("Wire frames chunks, answers HEAD and follows the request version") {
  campfire::Arena arena;
  Response response(arena.resource(), 200);
  response.chunked = true;
  response.body_view("abc");
  CHECK(wire_text(Wire(arena.resource(), response, {})) ==
        "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n");
  WireOptions head;
  head.head_only = true;
  head.http_minor = 0;
  Response plain(arena.resource(), 200);
  plain.add("content-length", "3");
  plain.body_view("abc");
  CHECK(wire_text(Wire(arena.resource(), plain, head)) == "HTTP/1.0 200 OK\r\ncontent-length: 3\r\n\r\n");
}

TEST_CASE("Wire skips bytes for a partial write") {
  campfire::Arena arena;
  Response response(arena.resource(), 200);
  response.body_view("0123456789");
  const Wire wire(arena.resource(), response, {});
  const std::string full = wire_text(wire);
  for (std::size_t skip = 0; skip <= full.size(); skip += 7) {
    iovec iov[64];
    const std::size_t n = wire.fill_iovecs(iov, skip);
    std::string rest;
    for (std::size_t i = 0; i < n; ++i) rest.append(static_cast<const char*>(iov[i].iov_base), iov[i].iov_len);
    CHECK(rest == full.substr(skip));
  }
}

TEST_CASE("TimerWheel fires in time, supports cancel and long delays") {
  TimerWheel wheel(0, 8);  // few slots: a long delay wraps
  TimerNode a;
  TimerNode b;
  TimerNode c;
  wheel.arm(a, 0, 100, 1);
  wheel.arm(b, 0, 1000, 2);  // more than one turn (8 * 50 ms)
  wheel.arm(c, 0, 100, 3);
  wheel.cancel(c);
  std::vector<std::uint32_t> fired;
  wheel.advance(120, [&](TimerNode& n) { fired.push_back(n.kind); });
  CHECK(fired == std::vector<std::uint32_t>{1});
  wheel.advance(900, [&](TimerNode& n) { fired.push_back(n.kind); });
  CHECK(fired.size() == 1);
  wheel.advance(1100, [&](TimerNode& n) { fired.push_back(n.kind); });
  CHECK(fired == std::vector<std::uint32_t>{1, 2});
  CHECK(wheel.size() == 0);
  CHECK(wheel.next_wait_ms(1100) == -1);
}

TEST_CASE("ThreadPool runs the jobs") {
  std::atomic<int> count{0};
  {
    ThreadPool pool(3);
    for (int i = 0; i < 100; ++i) pool.submit([&] { ++count; });
  }
  CHECK(count == 100);
}

TEST_CASE("front helpers") {
  char buffer[32];
  CHECK(format_http_date(1791217871, buffer) == "Mon, 05 Oct 2026 16:31:11 GMT");
  campfire::Arena arena;
  Response response(arena.resource(), 200);
  response.add("content-length", "1");
  Request request;
  apply_front_headers(request, response);
  CHECK(response.get("x-cache") == "miss");
  CHECK(response.get("vary") == "Accept-Encoding");
  Response posted(arena.resource(), 200);
  Request post;
  post.method = Method::Post;
  apply_front_headers(post, posted);
  CHECK(posted.get("x-cache") == "bypass");
  CHECK(posted.headers[0].name == "x-cache");
}

TEST_CASE("the JSON reader of the ACME client stops at a deep nesting") {
  using campfire::net::front::Json;
  const std::string deep = std::string(100000, '[') + std::string(100000, ']');
  CHECK_FALSE(Json::parse(deep));
  const std::string fine = std::string(32, '[') + std::string(32, ']');
  CHECK(Json::parse(fine));
  const auto order = Json::parse(R"({"status":"valid","challenges":[{"type":"http-01","token":"t"}]})");
  REQUIRE(order);
  CHECK(order->str("status") == "valid");
}
