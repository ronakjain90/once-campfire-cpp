// Tests of net/parser.hpp. Rust: hyper request parsing as used by crates/kit/src/front/conn.rs.
#include <doctest.h>

#include <string>

#include "net/parser.hpp"

using namespace campfire::net;

namespace {
struct Parsed {
  campfire::Arena arena;
  ParsedHead head;
  HeadResult result;
};
HeadResult parse(std::string_view text, campfire::Arena& arena, ParsedHead& head) {
  return parse_head(text, 0, arena, ParserLimits{}, head);
}
}  // namespace

TEST_CASE("parse_head reads a simple request") {
  campfire::Arena arena;
  ParsedHead head;
  const std::string text = "GET /rooms/1?x=y HTTP/1.1\r\nHost: a\r\nCookie: k=v\r\n\r\n";
  const HeadResult r = parse(text, arena, head);
  REQUIRE(r.status == HeadStatus::Ok);
  CHECK(head.head_size == text.size());
  CHECK(head.request.method == Method::Get);
  CHECK(head.request.path == "/rooms/1");
  CHECK(head.request.query == "x=y");
  CHECK(head.request.header("cookie") == "k=v");
  CHECK(head.request.keep_alive);
  CHECK(head.body_kind == BodyKind::None);
}

TEST_CASE("parse_head asks for more bytes and rejects bad input") {
  campfire::Arena arena;
  ParsedHead head;
  CHECK(parse("GET / HTTP/1.1\r\nHo", arena, head).status == HeadStatus::NeedMore);
  CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\n\r\n", arena, head).status == HeadStatus::Ok);
  CHECK(parse("BAD\r\n\r\n", arena, head).error_status == 400);
  CHECK(parse("GET x HTTP/1.1\r\n\r\n", arena, head).error_status == 400);
  CHECK(parse("POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", arena, head).error_status == 400);
  CHECK(parse("POST / HTTP/1.1\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n", arena, head).error_status == 400);
  CHECK(parse("POST / HTTP/1.1\r\nContent-Length: +1\r\n\r\n", arena, head).error_status == 400);
  CHECK(parse("POST / HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n", arena, head).error_status == 400);
  const std::string big = "GET / HTTP/1.1\r\nX: " + std::string(70000, 'a');
  CHECK(parse(big, arena, head).error_status == 431);
}

TEST_CASE("parse_head decides keep-alive and body kind") {
  campfire::Arena arena;
  ParsedHead head;
  REQUIRE(parse("GET / HTTP/1.0\r\n\r\n", arena, head).status == HeadStatus::Ok);
  CHECK_FALSE(head.request.keep_alive);
  REQUIRE(parse("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", arena, head).status == HeadStatus::Ok);
  CHECK(head.request.keep_alive);
  REQUIRE(parse("GET / HTTP/1.1\r\nConnection: Close\r\n\r\n", arena, head).status == HeadStatus::Ok);
  CHECK_FALSE(head.request.keep_alive);
  REQUIRE(parse("POST / HTTP/1.1\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n", arena, head).status ==
          HeadStatus::Ok);
  CHECK(head.body_kind == BodyKind::Length);
  CHECK(head.content_length == 5);
  CHECK(head.expect_continue);
  REQUIRE(parse("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", arena, head).status == HeadStatus::Ok);
  CHECK(head.body_kind == BodyKind::Chunked);
  REQUIRE(parse("GET http://h/a/b?q=1 HTTP/1.1\r\n\r\n", arena, head).status == HeadStatus::Ok);
  CHECK(head.request.path == "/a/b");
  CHECK(head.request.query == "q=1");
}

TEST_CASE("ChunkedBody decodes in place, in pieces, with pipelined bytes behind") {
  std::string buffer = "5\r\nhel";
  ChunkedBody body(0);
  std::size_t have = buffer.size();
  CHECK(body.feed(buffer.data(), have) == ChunkedBody::Status::NeedMore);
  buffer.resize(have);
  CHECK(buffer == "hel");
  buffer += "lo\r\n3\r\nabc\r\n0\r\n\r\nGET /";
  have = buffer.size();
  REQUIRE(body.feed(buffer.data(), have) == ChunkedBody::Status::Done);
  CHECK(std::string(buffer.data(), body.decoded()) == "helloabc");
  CHECK(std::string(buffer.data() + body.decoded(), body.leftover()) == "GET /");
}

TEST_CASE("ChunkedBody enforces the limit and rejects garbage") {
  std::string a = "6\r\nhello \r\n5\r\nworld\r\n0\r\n\r\n";
  ChunkedBody limited(10);
  std::size_t have = a.size();
  CHECK(limited.feed(a.data(), have) == ChunkedBody::Status::TooLarge);
  std::string b = "zz\r\n";
  ChunkedBody bad(0);
  have = b.size();
  CHECK(bad.feed(b.data(), have) == ChunkedBody::Status::Error);
}
