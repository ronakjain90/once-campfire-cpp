// Tests of cable/websocket.hpp. Rust: crates/cable/src/socket.rs tests.
#include "cable/websocket.hpp"
#include "cable/tests/support.hpp"

#include <doctest.h>

#include <string>
#include <vector>

using namespace campfire::cable::ws;

using campfire::cable::testing::client_frame;

TEST_CASE("handshake accept header matches RFC 6455") {
  std::span<const std::string_view> none;
  HandshakeRequest r{"13", "dGhlIHNhbXBsZSBub25jZQ==", none, none};
  auto h = accept_handshake(r);
  REQUIRE(h);
  CHECK(h->accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
  CHECK_FALSE(h->deflate);
  r.version = "8";
  CHECK_FALSE(accept_handshake(r));
}

TEST_CASE("single and fragmented messages") {
  FrameParser p;
  p.feed(client_frame(1, true, false, "hello"));
  auto m = p.next();
  REQUIRE((m && *m));
  CHECK((*m)->data == "hello");
  p.feed(client_frame(1, false, false, "he"));
  p.feed(client_frame(9, true, false, "pp"));
  p.feed(client_frame(0, true, false, "llo"));
  m = p.next();
  REQUIRE((m && *m));
  CHECK((*m)->type == Message::Type::Ping);
  m = p.next();
  REQUIRE((m && *m));
  CHECK((*m)->data == "hello");
}

TEST_CASE("unmasked frame closes with 1003") {
  FrameParser p;
  p.feed(encode_frame(Opcode::Text, false, "x"));
  auto m = p.next();
  REQUIRE_FALSE(m);
  CHECK(m.error() == 1003);
}

TEST_CASE("deflate round trip with zlib code") {
  std::string text(5000, 'a');
  auto d = deflate_message(text);
  auto back = inflate_message(d);
  REQUIRE(back);
  CHECK(*back == text);
}
