// RFC 6455 and RFC 7692 edge cases of cable/websocket.hpp, with zlib interop in both directions.
#include <doctest.h>
#include <zlib.h>

#include <random>
#include <string>

#include "cable/tests/support.hpp"
#include "cable/websocket.hpp"

using namespace campfire::cable::ws;
using campfire::cable::testing::client_frame;

namespace {

std::string bytes(std::initializer_list<int> list) {
  std::string s;
  for (int b : list) s.push_back(static_cast<char>(b));
  return s;
}

// The first message of a feed, or the close code.
std::expected<std::optional<Message>, std::uint16_t> parse_one(std::string_view data, ParserOptions o = {}) {
  FrameParser p(o);
  p.feed(data);
  return p.next();
}

std::uint16_t error_of(std::string_view data, ParserOptions o = {}) {
  auto r = parse_one(data, o);
  REQUIRE_FALSE(r);
  return r.error();
}

// Raw deflate with plain zlib, sync flush, tail removed (what a browser sends).
std::string zlib_deflate(std::string_view in, int level = 6) {
  z_stream s{};
  REQUIRE(deflateInit2(&s, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK);
  std::string out(deflateBound(&s, static_cast<uLong>(in.size())) + 64, '\0');
  s.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  s.avail_in = static_cast<uInt>(in.size());
  s.next_out = reinterpret_cast<Bytef*>(out.data());
  s.avail_out = static_cast<uInt>(out.size());
  REQUIRE(deflate(&s, Z_SYNC_FLUSH) == Z_OK);
  out.resize(s.total_out);
  deflateEnd(&s);
  REQUIRE(out.size() >= 4);
  REQUIRE(out.substr(out.size() - 4) == bytes({0, 0, 0xff, 0xff}));
  out.resize(out.size() - 4);
  return out;
}

// Inflates what the server sent, with plain zlib: the tail is added back.
std::string zlib_inflate(std::string_view in) {
  z_stream s{};
  REQUIRE(inflateInit2(&s, -15) == Z_OK);
  std::string data(in);
  data += bytes({0, 0, 0xff, 0xff});
  std::string out(1 << 20, '\0');
  s.next_in = reinterpret_cast<Bytef*>(data.data());
  s.avail_in = static_cast<uInt>(data.size());
  s.next_out = reinterpret_cast<Bytef*>(out.data());
  s.avail_out = static_cast<uInt>(out.size());
  int rc = inflate(&s, Z_SYNC_FLUSH);
  REQUIRE((rc == Z_OK || rc == Z_STREAM_END || rc == Z_BUF_ERROR));
  out.resize(s.total_out);
  inflateEnd(&s);
  return out;
}

}  // namespace

TEST_CASE("RFC 6455 section 5.7 examples") {
  // A single-frame masked text message.
  auto m = parse_one(bytes({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58}));
  REQUIRE((m && *m));
  CHECK((*m)->type == Message::Type::Text);
  CHECK((*m)->data == "Hello");
  // A masked ping and the unmasked pong.
  m = parse_one(bytes({0x89, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58}));
  REQUIRE((m && *m));
  CHECK((*m)->type == Message::Type::Ping);
  CHECK((*m)->data == "Hello");
  CHECK(encode_frame(Opcode::Pong, false, "Hello") == bytes({0x8a, 0x05, 'H', 'e', 'l', 'l', 'o'}));
  CHECK(encode_frame(Opcode::Text, false, "Hello") == bytes({0x81, 0x05, 'H', 'e', 'l', 'l', 'o'}));
  // A fragmented message: "Hel" and "lo".
  FrameParser p;
  p.feed(bytes({0x01, 0x83, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d}));
  p.feed(bytes({0x80, 0x82, 0x37, 0xfa, 0x21, 0x3d, 0x5b, 0x95}));
  auto r = p.next();
  REQUIRE((r && *r));
  CHECK((*r)->data == "Hello");
}

TEST_CASE("length encodings at the boundaries") {
  for (std::size_t n : {0u, 1u, 125u, 126u, 127u, 65535u, 65536u, 70000u}) {
    std::string payload(n, 'x');
    std::string frame = encode_frame(Opcode::Text, false, payload);
    std::size_t header = n < 126 ? 2 : (n <= 65535 ? 4 : 10);
    CHECK(frame.size() == header + n);
    CHECK(static_cast<std::uint8_t>(frame[1]) == (n < 126 ? n : (n <= 65535 ? 126 : 127)));
    auto m = parse_one(client_frame(1, true, false, payload));
    REQUIRE((m && *m));
    CHECK((*m)->data == payload);
  }
}

TEST_CASE("bytes in any chunking give the same messages") {
  std::string stream = client_frame(1, true, false, "one") + client_frame(9, true, false, "p") +
                       client_frame(1, false, false, "tw") + client_frame(0, true, false, "o") +
                       client_frame(2, true, false, "bin");
  std::vector<std::string> expected = {"one", "p", "two", "bin"};
  for (std::size_t chunk : {1u, 2u, 3u, 7u, 1000u}) {
    FrameParser p;
    std::vector<std::string> got;
    for (std::size_t i = 0; i < stream.size(); i += chunk) {
      p.feed(std::string_view(stream).substr(i, chunk));
      while (true) {
        auto r = p.next();
        REQUIRE(r);
        if (!*r) break;
        got.push_back((*r)->data);
      }
    }
    CHECK(got == expected);
    CHECK(p.buffered() == 0);
  }
}

TEST_CASE("a message is not ready before its last byte") {
  std::string frame = client_frame(1, true, false, std::string(300, 'a'));
  FrameParser p;
  p.feed(std::string_view(frame).substr(0, frame.size() - 1));
  auto r = p.next();
  REQUIRE(r);
  CHECK_FALSE(*r);
  p.feed(std::string_view(frame).substr(frame.size() - 1));
  r = p.next();
  REQUIRE((r && *r));
  CHECK((*r)->data.size() == 300);
}

TEST_CASE("protocol errors give the close code of websocket-driver") {
  CHECK(error_of(encode_frame(Opcode::Text, false, "x")) == 1003);                   // not masked
  CHECK(error_of(bytes({0x91, 0x80, 0, 0, 0, 0})) == 1002);                          // RSV3
  CHECK(error_of(bytes({0xa1, 0x80, 0, 0, 0, 0})) == 1002);                          // RSV2
  CHECK(error_of(client_frame(1, true, true, "x")) == 1002);                         // RSV1 without deflate
  CHECK(error_of(bytes({0x83, 0x80, 0, 0, 0, 0})) == 1002);                          // reserved opcode 3
  CHECK(error_of(bytes({0x8b, 0x80, 0, 0, 0, 0})) == 1002);                          // reserved opcode 0xb
  CHECK(error_of(client_frame(9, false, false, "")) == 1002);                        // fragmented control
  CHECK(error_of(client_frame(9, true, false, std::string(126, 'x'))) == 1002);      // long control
  CHECK(error_of(client_frame(0, true, false, "x")) == 1002);                        // continuation of nothing
  CHECK(error_of(client_frame(8, true, false, "x")) == 1002);                        // close with 1 byte
  CHECK(error_of(client_frame(8, true, false, bytes({0x03, 0xed}))) == 1002);        // 1005 on the wire
  CHECK(error_of(client_frame(8, true, false, bytes({0x03, 0xf7}))) == 1002);        // 1015
  CHECK(error_of(client_frame(8, true, false, bytes({0x03, 0xe8, 0xff}))) == 1002);  // reason not UTF-8
  CHECK(error_of(client_frame(1, true, false, bytes({0xc0, 0xaf}))) == 1007);        // overlong
  CHECK(error_of(client_frame(1, true, false, bytes({0xed, 0xa0, 0x80}))) == 1007);  // surrogate
}

TEST_CASE("a new data frame cannot interrupt a fragmented message") {
  FrameParser p;
  p.feed(client_frame(1, false, false, "a") + client_frame(1, true, false, "b"));
  auto r = p.next();
  REQUIRE_FALSE(r);
  CHECK(r.error() == 1002);
  // After an error the parser keeps the error.
  auto again = p.next();
  REQUIRE_FALSE(again);
  CHECK(again.error() == 1002);
}

TEST_CASE("control frames may sit between fragments") {
  FrameParser p;
  p.feed(client_frame(1, false, false, "a") + client_frame(10, true, false, "") + client_frame(0, false, false, "b") +
         client_frame(8, true, false, "") + client_frame(0, true, false, "c"));
  auto m = p.next();
  REQUIRE((m && *m));
  CHECK((*m)->type == Message::Type::Pong);
  m = p.next();
  REQUIRE((m && *m));
  CHECK((*m)->type == Message::Type::Close);
  CHECK_FALSE((*m)->close_code);
  m = p.next();
  REQUIRE((m && *m));
  CHECK((*m)->data == "abc");
}

TEST_CASE("a multi-byte character may split across fragments") {
  std::string euro = "\xe2\x82\xac";
  FrameParser p;
  p.feed(client_frame(1, false, false, euro.substr(0, 1)) + client_frame(0, true, false, euro.substr(1)));
  auto m = p.next();
  REQUIRE((m && *m));
  CHECK((*m)->data == euro);
  // But the whole message must be UTF-8, also when each fragment is.
  FrameParser q;
  q.feed(client_frame(1, false, false, euro.substr(0, 1)) + client_frame(0, true, false, "x"));
  auto r = q.next();
  REQUIRE_FALSE(r);
  CHECK(r.error() == 1007);
}

TEST_CASE("size limits close with 1009") {
  ParserOptions o;
  o.max_message = 100;
  CHECK(parse_one(client_frame(1, true, false, std::string(100, 'x')), o).has_value());
  CHECK(error_of(client_frame(1, true, false, std::string(101, 'x')), o) == 1009);
  // A fragment total over the limit.
  FrameParser p(o);
  p.feed(client_frame(1, false, false, std::string(60, 'x')) + client_frame(0, true, false, std::string(60, 'x')));
  auto r = p.next();
  REQUIRE_FALSE(r);
  CHECK(r.error() == 1009);
  // The header alone is enough: the server does not wait for 1 GiB.
  std::uint8_t head[10];
  std::size_t n = encode_header(head, Opcode::Text, false, std::size_t{1} << 30);
  std::string h(reinterpret_cast<char*>(head), n);
  h[1] = static_cast<char>(h[1] | 0x80);
  CHECK(error_of(h) == 1009);
}

TEST_CASE("a client parser accepts unmasked frames") {
  ParserOptions o;
  o.require_mask = false;
  auto m = parse_one(encode_frame(Opcode::Text, false, "from server"), o);
  REQUIRE((m && *m));
  CHECK((*m)->data == "from server");
}

TEST_CASE("masking is its own inverse for every length and offset") {
  const std::uint8_t mask[4] = {0xde, 0xad, 0xbe, 0xef};
  std::mt19937 rng(7);
  for (std::size_t n = 0; n < 40; ++n) {
    std::string a(n, '\0');
    for (auto& c : a) c = static_cast<char>(rng());
    std::string b = a;
    apply_mask(reinterpret_cast<std::uint8_t*>(b.data()), b.size(), mask);
    for (std::size_t i = 0; i < n; ++i)
      CHECK(static_cast<std::uint8_t>(b[i]) == (static_cast<std::uint8_t>(a[i]) ^ mask[i % 4]));
    apply_mask(reinterpret_cast<std::uint8_t*>(b.data()), b.size(), mask);
    CHECK(a == b);
  }
}

TEST_CASE("close frames") {
  CHECK(encode_close(1000) == bytes({0x88, 0x02, 0x03, 0xe8}));
  CHECK(encode_close_reply(std::nullopt) == bytes({0x88, 0x00}));
  CHECK(encode_close_reply(1001) == bytes({0x88, 0x02, 0x03, 0xe9}));
  auto m = parse_one(client_frame(8, true, false, bytes({0x0f, 0xa0}) + "bye"));  // 4000
  REQUIRE((m && *m));
  CHECK((*m)->close_code == 4000);
}

TEST_CASE("permessage-deflate: zlib compresses, the parser inflates") {
  ParserOptions o;
  o.deflate = true;
  std::mt19937 rng(1);
  for (std::size_t n : {0u, 1u, 10u, 1000u, 100000u}) {
    std::string text;
    for (std::size_t i = 0; i < n; ++i) text.push_back(static_cast<char>('a' + rng() % (n > 1000 ? 4 : 26)));
    std::string frame = client_frame(1, true, true, zlib_deflate(text));
    auto m = parse_one(frame, o);
    REQUIRE((m && *m));
    CHECK((*m)->data == text);
  }
}

TEST_CASE("permessage-deflate: our compressor, zlib inflates") {
  std::mt19937 rng(2);
  for (std::size_t n : {0u, 1u, 300u, 5000u, 200000u}) {
    std::string text;
    for (std::size_t i = 0; i < n; ++i) text.push_back(static_cast<char>('a' + rng() % 5));
    CHECK(zlib_inflate(deflate_message(text)) == text);
  }
  // The same bytes for the same input: no context takeover.
  CHECK(deflate_message("hello hello hello") == deflate_message("hello hello hello"));
}

TEST_CASE("a compressed message in fragments, and with an empty block") {
  ParserOptions o;
  o.deflate = true;
  std::string text(2000, 'q');
  std::string z = zlib_deflate(text);
  FrameParser p(o);
  p.feed(client_frame(1, false, true, z.substr(0, z.size() / 2)) +
         client_frame(0, true, false, z.substr(z.size() / 2)));
  auto m = p.next();
  REQUIRE((m && *m));
  CHECK((*m)->data == text);
  // RSV1 on a continuation frame is an error.
  CHECK(error_of(client_frame(1, false, false, "a") + client_frame(0, true, true, "b"), o) == 1002);
  // RSV1 on a control frame is an error.
  CHECK(error_of(client_frame(9, true, true, ""), o) == 1002);
}

TEST_CASE("bad deflate data and decompression bombs") {
  ParserOptions o;
  o.deflate = true;
  CHECK(error_of(client_frame(1, true, true, bytes({0xff, 0xff, 0xff, 0xff})), o) == 1002);
  o.max_message = 2000;
  std::string bomb = zlib_deflate(std::string(1 << 20, 'a'), 9);
  REQUIRE(bomb.size() < 2000);
  CHECK(error_of(client_frame(1, true, true, bomb), o) == 1009);
  // Inflated text that is not UTF-8.
  o.max_message = kMaxMessage;
  CHECK(error_of(client_frame(1, true, true, zlib_deflate(bytes({0xff, 0xfe}))), o) == 1007);
}

TEST_CASE("handshake negotiation") {
  std::string_view key = "dGhlIHNhbXBsZSBub25jZQ==";
  auto run = [&](std::vector<std::string_view> ext, std::vector<std::string_view> proto) {
    HandshakeRequest r{"13", key, ext, proto};
    return accept_handshake(r);
  };
  CHECK(run({"permessage-deflate"}, {})->deflate);
  CHECK(run({"permessage-deflate; client_max_window_bits"}, {})->deflate);
  CHECK(run({"permessage-deflate; client_max_window_bits=12"}, {})->deflate);
  CHECK(run({"permessage-deflate; server_no_context_takeover; client_no_context_takeover"}, {})->deflate);
  CHECK(run({"permessage-deflate; server_max_window_bits=15"}, {})->deflate);
  CHECK_FALSE(run({"permessage-deflate; server_max_window_bits=10"}, {})->deflate);
  CHECK_FALSE(run({"permessage-deflate; bogus"}, {})->deflate);
  CHECK_FALSE(run({"x-webkit-deflate-frame"}, {})->deflate);
  CHECK(run({"foo, permessage-deflate"}, {})->deflate);
  CHECK(run({"foo", "permessage-deflate"}, {})->deflate);
  CHECK(run({}, {"actioncable-v1-json, actioncable-unsupported"})->protocol == "actioncable-v1-json");
  CHECK(run({}, {"actioncable-unsupported"})->protocol == "actioncable-unsupported");
  CHECK(run({}, {"graphql-ws"})->protocol.empty());
  CHECK_FALSE(accept_handshake(HandshakeRequest{"13", "", {}, {}}));
  CHECK_FALSE(accept_handshake(HandshakeRequest{"13", "c2hvcnQ=", {}, {}}));
  CHECK_FALSE(accept_handshake(HandshakeRequest{"", key, {}, {}}));
  auto head = run({"permessage-deflate"}, {"actioncable-v1-json"})->response_head();
  CHECK(head ==
        "HTTP/1.1 101 Switching Protocols\r\nupgrade: websocket\r\nconnection: upgrade\r\n"
        "sec-websocket-accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "sec-websocket-extensions: permessage-deflate; server_no_context_takeover; client_no_context_takeover\r\n"
        "sec-websocket-protocol: actioncable-v1-json\r\n\r\n");
}

TEST_CASE("upgrade request detection") {
  std::vector<std::string_view> c1{"keep-alive, Upgrade"};
  std::vector<std::string_view> c2{"keep-alive", "upgrade"};
  std::vector<std::string_view> c3{"close"};
  CHECK(is_upgrade_request("GET", c1, "WebSocket"));
  CHECK(is_upgrade_request("GET", c2, "websocket"));
  CHECK_FALSE(is_upgrade_request("GET", c3, "websocket"));
  CHECK_FALSE(is_upgrade_request("POST", c1, "websocket"));
  CHECK_FALSE(is_upgrade_request("GET", c1, "h2c"));
}
