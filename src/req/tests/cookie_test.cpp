// Tests of the cookie jar, the session and the flash. The cases come from crates/kit/src/cookies.rs
// and session.rs.
#include "req/cookie.hpp"

#include <doctest.h>

#include <chrono>
#include <string>
#include <vector>

#include "compat/cookies.hpp"
#include "core/time_format.hpp"
#include "req/session.hpp"

using namespace campfire;
using namespace campfire::req;
using Strings = std::vector<std::string>;

namespace {

const compat::Secrets& secrets() {
  static const compat::Secrets s("test-secret");
  return s;
}

std::shared_ptr<TestClock> clock_at_2024() {
  static auto c = TestClock::frozen_at(*parse_rfc3339("2024-06-01T12:00:00Z"));
  return c;
}

CookieJar jar(std::string_view header) {
  return CookieJar(std::vector<std::string_view>{header}, secrets(), *clock_at_2024());
}

Cookie C(std::string value, bool permanent = false, bool httponly = false, bool secure = false) {
  Cookie c;
  c.value = std::move(value);
  c.permanent = permanent;
  c.httponly = httponly;
  c.secure = secure;
  return c;
}

}  // namespace

TEST_CASE("cookie: the Cookie header") {
  auto j = jar("a=1; b=x%20y+z;c=3; a=2; d");
  CHECK(j.get("a") == "1");
  CHECK(j.get("b") == "x y z");
  CHECK(j.get("c") == "3");
  CHECK(j.get("d") == "");
  CHECK_FALSE(j.get("zz").has_value());
  // A value that does not unescape stays as it is.
  CHECK(jar("a=%zz").get("a") == "%zz");
  CHECK(jar("a=%FF").get("a") == "%FF");
}

TEST_CASE("cookie: two Cookie headers, the first value wins") {
  CookieJar j({"a=1", "a=2; b=3"}, secrets(), *clock_at_2024());
  CHECK(j.get("a") == "1");
  CHECK(j.get("b") == "3");
}

TEST_CASE("cookie: plain cookies have the Rails defaults") {
  auto j = jar("");
  j.set("last_room", C("42"));
  CHECK(j.set_cookie_headers(false, "example.com") == Strings{"last_room=42; path=/; samesite=lax"});
}

TEST_CASE("cookie: an unchanged value is not written unless it expires") {
  auto j = jar("last_room=42");
  j.set("last_room", C("42"));
  CHECK(j.set_cookie_headers(false, "h").empty());
  j.set("last_room", C("42", true));
  CHECK(j.set_cookie_headers(false, "h") ==
        Strings{"last_room=42; path=/; expires=Wed, 01 Jun 2044 12:00:00 GMT; samesite=lax"});
}

TEST_CASE("cookie: values are escaped") {
  auto j = jar("");
  j.set("x", C("a b+c/="));
  CHECK(j.set_cookie_headers(false, "h") == Strings{"x=a+b%2Bc%2F%3D; path=/; samesite=lax"});
}

TEST_CASE("cookie: signed, permanent, httponly round trip") {
  auto j = jar("");
  REQUIRE(j.set_signed("session_token", C("tok", true, true)).has_value());
  const auto headers = j.set_cookie_headers(true, "h");
  REQUIRE(headers.size() == 1);
  CHECK(headers[0].starts_with("session_token="));
  CHECK(headers[0].ends_with("; path=/; expires=Wed, 01 Jun 2044 12:00:00 GMT; httponly; samesite=lax"));
  CHECK(j.signed_value("session_token") == "tok");

  const std::string raw(*j.get("session_token"));
  auto next = jar("session_token=" + compat::cookies::escape(raw));
  CHECK(next.signed_value("session_token") == "tok");
}

TEST_CASE("cookie: a forged signed cookie reads as nil") {
  auto j = jar("session_token=forged");
  CHECK_FALSE(j.signed_value("session_token").has_value());
  CHECK(j.get("session_token") == "forged");
}

TEST_CASE("cookie: an encrypted value round trips") {
  auto j = jar("");
  const compat::json::Value value(compat::json::Value::Object{{"a", compat::json::Value(1)}});
  REQUIRE(j.set_encrypted("secret", value, Cookie{}).has_value());
  CHECK(j.encrypted_value("secret") == value);
}

TEST_CASE("cookie: a value over 4096 bytes overflows") {
  auto j = jar("");
  auto status = j.set_signed("big", C(std::string(kMaxCookieSize, 'x')));
  CHECK_FALSE(status.has_value());
}

TEST_CASE("cookie: delete only when present") {
  auto j = jar("session_token=abc");
  j.remove("missing");
  j.remove("session_token");
  CHECK(j.is_deleted("session_token"));
  CHECK_FALSE(j.get("session_token").has_value());
  CHECK(j.set_cookie_headers(false, "h") ==
        Strings{"session_token=; path=/; max-age=0; expires=Thu, 01 Jan 1970 00:00:00 GMT; samesite=lax"});
}

TEST_CASE("cookie: a secure cookie needs SSL") {
  auto j = jar("");
  j.set("s", C("1", false, false, true));
  CHECK(j.set_cookie_headers(false, "example.com").empty());
  CHECK(j.set_cookie_headers(false, "x.onion").size() == 1);
  CHECK(j.set_cookie_headers(true, "example.com") == Strings{"s=1; path=/; secure; samesite=lax"});
}

TEST_CASE("cookie: 100000 cookies parse in linear time") {
  std::string header;
  for (int i = 0; i < 100000; ++i) header += (i ? "; c" : "c") + std::to_string(i) + "=1";
  const auto start = std::chrono::steady_clock::now();
  CHECK(parse_cookie_header(header).size() == 100000);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
}

TEST_CASE("session: written only on change and deleted when empty") {
  auto j = jar("");
  Session session;
  session.load(j);
  CHECK(session.id()->size() == 32);
  REQUIRE(session.commit(j, clock_at_2024()->now()).has_value());
  CHECK(j.set_cookie_headers(true, "h").empty());

  session.insert("return_to", compat::json::Value("/rooms/1"));
  REQUIRE(session.commit(j, clock_at_2024()->now()).has_value());
  const auto headers = j.set_cookie_headers(true, "h");
  REQUIRE(headers.size() == 1);
  CHECK(headers[0].starts_with("_campfire_session="));
  CHECK(headers[0].ends_with("; path=/; expires=Wed, 01 Jun 2044 12:00:00 GMT; httponly; samesite=lax"));

  // The next request reads the cookie.
  auto next = jar(headers[0].substr(0, headers[0].find(';')));
  Session again;
  again.load(next);
  CHECK(again.get_str("return_to") == "/rooms/1");
  CHECK(again.id() == session.id());

  // The last value goes: the cookie goes too.
  again.remove("return_to");
  REQUIRE(again.commit(next, clock_at_2024()->now()).has_value());
  CHECK(next.is_deleted("_campfire_session"));
}

TEST_CASE("session: the id is made only when it is read or written, as the first key") {
  auto j = jar("");
  Session session;
  session.load(j);
  CHECK_FALSE(session.contains_key("session_id"));  // reading the session alone makes no id
  CHECK(session.get("flash") == nullptr);
  CHECK_FALSE(session.contains_key("session_id"));

  // Written without a read of the id: the id is made, and it comes first, as Rails writes it.
  session.insert("return_to", compat::json::Value("/rooms/1"));
  REQUIRE(session.commit(j, clock_at_2024()->now()).has_value());
  const auto headers = j.set_cookie_headers(true, "h");
  REQUIRE(headers.size() == 1);
  auto next = jar(headers[0].substr(0, headers[0].find(';')));
  const auto stored = next.encrypted_value("_campfire_session");
  REQUIRE(stored.has_value());
  REQUIRE(stored->is_object());
  REQUIRE(stored->as_object().size() == 2);
  CHECK(stored->as_object()[0].first == "session_id");
  CHECK(stored->as_object()[0].second.as_string().size() == 32);
  CHECK(stored->as_object()[1].first == "return_to");
}

TEST_CASE("flash: shown once") {
  Flash flash;
  flash.set("notice", compat::json::Value("ok"));
  const auto stored = flash.to_session_value();
  REQUIRE(stored.has_value());
  CHECK(compat::json::generate(*stored) == R"({"discard":[],"flashes":{"notice":"ok"}})");
  auto next = Flash::from_session_value(&*stored);
  CHECK(next.notice() == "ok");
  CHECK_FALSE(next.to_session_value().has_value());
}

TEST_CASE("flash: now, keep and a legacy discard list") {
  Flash flash;
  flash.now("alert", compat::json::Value("x"));
  CHECK(flash.alert() == "x");
  CHECK_FALSE(flash.to_session_value().has_value());

  auto stored = compat::json::parse(R"({"discard":["alert"],"flashes":{"notice":"hi","alert":"gone"}})");
  auto loaded = Flash::from_session_value(&*stored);
  CHECK_FALSE(loaded.alert().has_value());
  loaded.keep("notice");
  CHECK(compat::json::generate(*loaded.to_session_value()) == R"({"discard":[],"flashes":{"notice":"hi"}})");
}
