// Tests of net/router.hpp. Rails: config/routes.rb matching.
#include <doctest.h>

#include "net/router.hpp"

namespace campfire::app_test {
const net::RouteTable& routes();
}

namespace campfire::routes::test {
#define H(name) \
  Task<net::Response> name(net::Ctx& ctx) { co_return ctx.response(200); }
H(root) H(rooms_index) H(rooms_create) H(rooms_opens) H(rooms_show) H(messages_index) H(at_message)
H(bot_messages) H(manifest) H(asset) H(profile)
#undef H
}  // namespace campfire::routes::test

using namespace campfire::net;

namespace {
Match find(Method m, std::string_view path) { return match_route(campfire::app_test::routes(), m, path); }
}  // namespace

TEST_CASE("router matches literals, order and trailing slash") {
  CHECK(find(Method::Get, "/").route->name == "test::root");
  CHECK(find(Method::Get, "/rooms").route->name == "test::rooms_index");
  CHECK(find(Method::Get, "/rooms/").route->name == "test::rooms_index");
  CHECK(find(Method::Post, "/rooms").route->name == "test::rooms_create");
  CHECK(find(Method::Get, "/rooms/opens").route->name == "test::rooms_opens");  // before :id
  CHECK_FALSE(find(Method::Delete, "/rooms"));
  CHECK_FALSE(find(Method::Get, "/nope"));
  CHECK_FALSE(find(Method::Get, "/rooms//messages"));
  CHECK_FALSE(find(Method::Other, "/"));
  CHECK_FALSE(find(Method::Get, "rooms"));
}

TEST_CASE("router sets path params and format") {
  Match m = find(Method::Get, "/rooms/42");
  REQUIRE(m);
  CHECK(m.route->name == "test::rooms_show");
  CHECK(m.params.get("id") == "42");
  CHECK_FALSE(m.params.has("format"));
  m = find(Method::Get, "/rooms/42.json");
  REQUIRE(m);
  CHECK(m.params.get("id") == "42");
  CHECK(m.params.get("format") == "json");
  CHECK_FALSE(find(Method::Get, "/rooms/4.2.json"));
  m = find(Method::Get, "/rooms/7/messages.json");
  REQUIRE(m);
  CHECK(m.params.get("room_id") == "7");
  CHECK(m.params.get("format") == "json");
  m = find(Method::Get, "/webmanifest.json");
  REQUIRE(m);
  CHECK(m.params.get("format") == "json");
  CHECK(find(Method::Get, "/webmanifest"));
  CHECK_FALSE(find(Method::Get, "/rooms/7/messages.json.x"));
}

TEST_CASE("router handles the @ route, globs, defaults and HEAD") {
  Match m = find(Method::Get, "/rooms/3/@15");
  REQUIRE(m);
  CHECK(m.route->name == "test::at_message");
  CHECK(m.params.get("message_id") == "15");
  CHECK_FALSE(find(Method::Get, "/rooms/3/@"));
  m = find(Method::Get, "/rooms/3/botkey/messages");
  REQUIRE(m);
  CHECK(m.params.get("bot_key") == "botkey");
  CHECK(m.params.get("format") == "json");  // default
  m = find(Method::Get, "/assets/a/b/c.js");
  REQUIRE(m);
  CHECK(m.params.get("path") == "a/b/c.js");
  CHECK_FALSE(find(Method::Get, "/assets"));
  m = find(Method::Get, "/users/9/profile");
  CHECK(m.params.get("user_id") == "9");
  m = find(Method::Get, "/users/me/profile");
  CHECK(m.params.get("user_id") == "me");
  m = find(Method::Head, "/rooms/42");
  REQUIRE(m);
  CHECK(m.route->method == Method::Get);
}
