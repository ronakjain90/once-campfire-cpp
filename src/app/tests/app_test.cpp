// End to end tests of the sign-in routes on a real server. Rails: SessionsController and the concerns.
#include <doctest.h>

#include "app/tests/fixture.hpp"

namespace campfire::app::testing {

namespace {

std::string cookie_pair(const Reply& reply, const std::string& name) {
  for (const auto& [k, v] : reply.headers) {
    if (k == "set-cookie" && v.starts_with(name + "=")) return v.substr(0, v.find(';'));
  }
  return {};
}

std::string form(const std::string& email, const std::string& password) {
  return "email_address=" + email + "&password=" + password;
}

}  // namespace

TEST_CASE("sign-in page: headers, layout and form") {
  Fixture f;
  Client c(f.port());
  const Reply r = c.request("GET", "/session/new", "Accept: */*\r\n");
  REQUIRE(r.status == 200);
  CHECK(r.header("content-type") == "text/html; charset=utf-8");
  CHECK(r.header("vary") == "Accept,Accept-Encoding");
  CHECK(r.header("x-version") == "test");
  CHECK(r.header("x-rev") == "rev");
  CHECK(r.header("x-frame-options") == "SAMEORIGIN");
  CHECK(r.header("cache-control") == "max-age=0, private, must-revalidate");
  CHECK(r.header("etag").starts_with("W/\""));
  CHECK(r.header("etag").size() == 2 + 1 + 32 + 1);
  CHECK(r.header("link").find("rel=preload; as=style; nopush") != std::string::npos);
  CHECK(r.body.starts_with("<!DOCTYPE html>"));
  CHECK(r.body.find("<title>Sign in</title>") != std::string::npos);
  CHECK(r.body.find("<strong>37signals</strong>") != std::string::npos);
  CHECK(r.body.find("david@example.com") != std::string::npos);  // the help contact
  CHECK(r.body.find("class=\"panel \"") != std::string::npos);
  // The order of the headers is part of the bytes.
  const auto names = r.header_names();
  const auto at = [&](const char* n) { return std::find(names.begin(), names.end(), n) - names.begin(); };
  CHECK(at("content-type") < at("vary"));
  CHECK(at("vary") < at("x-version"));
  CHECK(at("x-rev") < at("link"));
  CHECK(at("link") < at("x-frame-options"));
  CHECK(at("etag") < at("cache-control"));
  CHECK(at("cache-control") < at("content-length"));
  CHECK(at("content-length") < at("x-request-id"));
  CHECK(at("x-request-id") < at("x-runtime"));
}

TEST_CASE("sign in, resume, sign out") {
  Fixture f;
  Client c(f.port());
  // An unauthenticated request goes to the sign-in page and remembers where it came from.
  Reply r = c.request("GET", "/?x=1");
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/session/new");
  CHECK(!cookie_pair(r, "_campfire_session").empty());
  const std::string guest = cookie_pair(r, "_campfire_session");

  // A wrong password gives the rejection page.
  r = c.request("POST", "/session", kSameOrigin + kForm + "Cookie: " + guest + "\r\n",
                form("david@example.com", "wrong"));
  CHECK(r.status == 401);
  CHECK(r.body.find("Too many requests or unauthorized.") != std::string::npos);
  CHECK(r.body.find("class=\"panel shake\"") != std::string::npos);
  CHECK(r.body.find("value=\"david@example.com\"") != std::string::npos);
  CHECK(r.header("etag").empty());
  CHECK(r.header("cache-control") == "no-cache");

  // The right password starts a session and goes back to where the visitor came from.
  r = c.request("POST", "/session", kSameOrigin + kForm + "Cookie: " + guest + "\r\n",
                form("david@example.com", kPassword));
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/?x=1");
  const std::string token = cookie_pair(r, "session_token");
  REQUIRE(!token.empty());
  CHECK(r.header("set-cookie").find("httponly") != std::string::npos);
  CHECK(r.header("set-cookie").find("samesite=lax") != std::string::npos);

  // The session works (a user with no rooms gets the welcome page).
  r = c.request("GET", "/", "Cookie: " + token + "\r\n");
  CHECK(r.status == 200);
  CHECK(r.body.find("No rooms yet") != std::string::npos);
  r = c.request("GET", "/", "Cookie: " + token + "\r\n");
  CHECK(r.status == 200);

  // Sign out destroys the session and deletes the cookie.
  r = c.request("DELETE", "/session", kSameOrigin + "Cookie: " + token + "\r\n");
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/");
  CHECK(r.header("set-cookie").starts_with("session_token=;"));
  CHECK(r.header("set-cookie").find("max-age=0") != std::string::npos);
  r = c.request("GET", "/", "Cookie: " + token + "\r\n");
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/session/new");
}

TEST_CASE("sign-in: unknown user, forged request, rate limit") {
  Fixture f;
  Client c(f.port());
  Reply r = c.request("POST", "/session", kSameOrigin + kForm, form("nobody@example.com", "x"));
  CHECK(r.status == 401);
  // A cross-site POST is refused with the public 422 page.
  r = c.request("POST", "/session", "Sec-Fetch-Site: cross-site\r\n" + kForm, form("david@example.com", kPassword));
  CHECK(r.status == 422);
  CHECK(r.header("content-type") == "text/html; charset=UTF-8");
  CHECK(r.header("x-version").empty());
  // Ten attempts in 3 minutes. The 422 did not count: the forgery check runs first. Attempt 1 was above.
  int rejected = 0;
  for (int i = 0; i < 12; ++i) {
    r = c.request("POST", "/session", kSameOrigin + kForm, form("david@example.com", "wrong"));
    if (r.status == 429) ++rejected;
  }
  CHECK(rejected == 3);  // the attempts 11, 12 and 13 of 13
  f.clock->travel(181);
  r = c.request("POST", "/session", kSameOrigin + kForm, form("david@example.com", "wrong"));
  CHECK(r.status == 401);
}

TEST_CASE("errors: not found, JSON, HEAD") {
  Fixture f;
  Client c(f.port());
  Reply r = c.request("GET", "/nope");
  CHECK(r.status == 404);
  CHECK(r.header("content-length") == "4237");
  r = c.request("GET", "/nope.json");
  CHECK(r.status == 404);
  CHECK(r.body == R"({"status":404,"error":"Not Found"})");
  // The sign-in page does not offer JSON.
  r = c.request("GET", "/session/new", "Accept: application/json\r\n");
  CHECK(r.status == 406);
  CHECK(r.body == R"({"status":406,"error":"Not Acceptable"})");
  r = c.request("HEAD", "/session/new");
  CHECK(r.status == 200);
  CHECK(!r.header("content-length").empty());
}

}  // namespace campfire::app::testing
