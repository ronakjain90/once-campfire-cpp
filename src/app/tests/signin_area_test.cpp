// End to end tests of the area A1: first run, join links, session transfers, welcome, allow_browser, bans.
// Rails: first_runs, users (new, create), sessions/transfers, welcome controllers and the concerns.
#include <doctest.h>

#include "app/tests/fixture.hpp"
#include "compat/signed_id.hpp"

namespace campfire::app::testing {

namespace {

std::string cookie_pair(const Reply& reply, const std::string& name) {
  for (const auto& [k, v] : reply.headers) {
    if (k == "set-cookie" && v.starts_with(name + "=")) return v.substr(0, v.find(';'));
  }
  return {};
}

const db::Query<void(std::string_view, std::string_view)> kInsertRoom{
    "INSERT INTO rooms (name, type, creator_id, created_at, updated_at) VALUES ('Room', 'Rooms::Open', 1, ?, ?)"};
const db::Query<void(std::int64_t, std::int64_t)> kInsertMembership{
    "INSERT INTO memberships (room_id, user_id, created_at, updated_at) VALUES (?, ?, '2026-03-02 16:00:00', "
    "'2026-03-02 16:00:00')"};
const db::Query<void(std::int64_t)> kDeactivate{"UPDATE users SET status = 1 WHERE id = ?"};
const db::Query<void(std::string_view)> kInsertBan{
    "INSERT INTO bans (user_id, ip_address, created_at, updated_at) VALUES (1, ?, '2026-03-02 16:00:00', "
    "'2026-03-02 16:00:00')"};

std::string sign_in(Client& c, const std::string& email) {
  const Reply r =
      c.request("POST", "/session", kSameOrigin + kForm, "email_address=" + email + "&password=" + kPassword);
  REQUIRE(r.status == 302);
  return cookie_pair(r, "session_token");
}

const std::string kChrome124 =
    "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/124.0.0.0 Safari/537.36\r\n";

}  // namespace

TEST_CASE("welcome: the page without rooms, then the redirect to the last room") {
  Fixture f;
  Client c(f.port());
  const std::string token = sign_in(c, "david@example.com");
  Reply r = c.request("GET", "/", "Cookie: " + token + "\r\n");
  CHECK(r.status == 200);
  CHECK(r.body.find("<title>No rooms yet</title>") != std::string::npos);
  CHECK(r.body.find("<body class=\"sidebar admin\"") != std::string::npos);
  CHECK(r.body.find("<turbo-frame data-turbo-permanent=\"true\"") != std::string::npos);
  CHECK(r.body.find("id=\"user_sidebar\" src=\"/users/me/sidebar\" target=\"_top\"") != std::string::npos);

  f.write([&](db::Tx& tx) -> Status {
    // Room 2 is older than room 1.
    for (const char* created : {"2026-03-02 16:00:00", "2026-03-01 16:00:00"}) {
      if (auto done = tx.conn().exec(kInsertRoom, created, created); !done) return std::unexpected(done.error());
    }
    // David (id 1) is in both rooms.
    for (const std::int64_t room : {2, 1}) {
      if (auto done = tx.conn().exec(kInsertMembership, room, 1); !done) return std::unexpected(done.error());
    }
    return {};
  });
  r = c.request("GET", "/", "Cookie: " + token + "\r\n");
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/rooms/2");  // the oldest room
  r = c.request("GET", "/", "Cookie: " + token + "; last_room=1\r\n");
  CHECK(r.header("location") == "http://test.example/rooms/1");
  r = c.request("GET", "/", "Cookie: " + token + "; last_room=3\r\n");  // not a room of the user
  CHECK(r.header("location") == "http://test.example/rooms/2");
  r = c.request("GET", "/", "Cookie: " + token + "; last_room=abc\r\n");
  CHECK(r.header("location") == "http://test.example/rooms/2");
}

TEST_CASE("first run: the form, the account, the first room and the redirects") {
  Fixture f({}, false);
  Client c(f.port());
  Reply r = c.request("GET", "/session/new");
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/first_run");
  r = c.request("GET", "/first_run");
  REQUIRE(r.status == 200);
  CHECK(r.body.find("<title>Set up Campfire</title>") != std::string::npos);
  CHECK(r.body.find("<body class=\"signup\"") != std::string::npos);
  CHECK(r.body.find("<form class=\"center max-width\" enctype=\"multipart/form-data\" action=\"/first_run\" "
                    "accept-charset=\"UTF-8\" method=\"post\">") != std::string::npos);
  CHECK(r.body.find("name=\"user[email_address]\"") != std::string::npos);
  for (const char* verb : {"GET /first_run/new", "GET /first_run/edit"}) {
    const std::string line = verb;
    r = c.request("GET", line.substr(4));
    CHECK(r.status == 404);
  }
  r = c.request("DELETE", "/first_run", kSameOrigin);
  CHECK(r.status == 404);

  r = c.request("POST", "/first_run", kSameOrigin + kForm, "user[name]=Owner&user[email_address]=owner%40example.com");
  CHECK(r.status == 302);  // no password: the user has no digest, but the first run goes on
  r = c.request("GET", "/first_run");
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/");

  Fixture g({}, false);
  Client d(g.port());
  r = d.request("POST", "/first_run", kSameOrigin + kForm,
                "user[name]=Owner&user[email_address]=owner%40example.com&user[password]=secret123456");
  REQUIRE(r.status == 302);
  CHECK(r.header("location") == "http://test.example/");
  const std::string token = cookie_pair(r, "session_token");
  REQUIRE(!token.empty());
  r = d.request("GET", "/", "Cookie: " + token + "\r\n");
  CHECK(r.status == 302);  // the owner is in the first room
  CHECK(r.header("location") == "http://test.example/rooms/1");
  r = d.request("GET", "/session/new");
  CHECK(r.status == 200);
  CHECK(r.body.find("<strong>Campfire</strong>") != std::string::npos);
  r = d.request("POST", "/first_run", kSameOrigin + kForm, "user[name]=X");
  CHECK(r.status == 302);  // an account exists
  r = d.request("POST", "/session", kSameOrigin + kForm, "email_address=owner%40example.com&password=secret123456");
  CHECK(r.status == 302);
}

TEST_CASE("first run: a missing user or name") {
  Fixture f({}, false);
  Client c(f.port());
  Reply r = c.request("POST", "/first_run", kSameOrigin + kForm, "x=1");
  CHECK(r.status == 400);  // ActionController::ParameterMissing
  r = c.request("POST", "/first_run", kSameOrigin + kForm, "user[email_address]=a%40b.c");
  CHECK(r.status == 500);  // users.name is NOT NULL
}

TEST_CASE("join: the form, the code, the new user and a duplicate address") {
  Fixture f;
  Client c(f.port());
  Reply r = c.request("GET", "/join/abcd-efgh-ijkl");
  REQUIRE(r.status == 200);
  CHECK(r.body.find("<title>Sign up</title>") != std::string::npos);
  CHECK(r.body.find("action=\"/join/abcd-efgh-ijkl\"") != std::string::npos);
  CHECK(r.body.find("<strong class=\"txt-large\">37signals</strong>") != std::string::npos);
  CHECK(r.body.find("href=\"/session/new\"") != std::string::npos);  // the nav
  r = c.request("GET", "/join/nope");
  CHECK(r.status == 404);
  CHECK(r.body.empty());
  r = c.request("POST", "/join/nope", kSameOrigin + kForm, "user[name]=X");
  CHECK(r.status == 404);

  f.write([&](db::Tx& tx) -> Status {
    if (auto done = tx.conn().exec(kInsertRoom, "2026-03-02 16:00:00", "2026-03-02 16:00:00"); !done) {
      return std::unexpected(done.error());
    }
    return {};
  });
  r = c.request("POST", "/join/abcd-efgh-ijkl", kSameOrigin + kForm,
                "user[name]=New+Person&user[email_address]=new%40example.com&user[password]=secret123456");
  REQUIRE(r.status == 302);
  CHECK(r.header("location") == "http://test.example/");
  const std::string token = cookie_pair(r, "session_token");
  REQUIRE(!token.empty());
  // The new user is a member of the open room.
  r = c.request("GET", "/", "Cookie: " + token + "\r\n");
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/rooms/1");
  // A signed in user goes to the root.
  r = c.request("GET", "/join/abcd-efgh-ijkl", "Cookie: " + token + "\r\n");
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/");
  // The address is taken.
  Client d(f.port());
  r = d.request("POST", "/join/abcd-efgh-ijkl", kSameOrigin + kForm,
                "user[name]=Again&user[email_address]=new%40example.com");
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/session/new?email_address=new%40example.com");
  r = d.request("POST", "/join/abcd-efgh-ijkl", kSameOrigin + kForm, "user[name]=Again");
  CHECK(r.status == 302);  // no email address: it is NULL, which a unique index allows
  CHECK(r.header("location") == "http://test.example/");
}

TEST_CASE("session transfer: the page, the sign in, a bad id and a deactivated user") {
  Fixture f;
  Client c(f.port());
  const Timestamp now = f.clock->now();
  const auto id_for = [&](std::int64_t user_id, std::int64_t life_seconds) {
    const compat::Timestamp at(std::chrono::nanoseconds((now.seconds + life_seconds) * 1'000'000'000LL));
    return compat::signed_id::generate(f.state->secrets, "User", user_id, "transfer", at);
  };
  const std::string good = id_for(1, 4 * 3600);
  Reply r = c.request("GET", "/session/transfers/" + good);
  REQUIRE(r.status == 200);
  CHECK(r.body.find("action=\"/session/transfers/" + good + "\"") != std::string::npos);
  CHECK(r.body.find("name=\"_method\" value=\"put\"") != std::string::npos);
  // Rails 27f5461: the form has an empty block, so it ends with `</form>`.
  CHECK(r.body.find("name=\"_method\" value=\"put\" />\n</form>") != std::string::npos);
  r = c.request("PATCH", "/session/transfers/" + id_for(1, -10), kSameOrigin);  // expired
  CHECK(r.status == 400);
  r = c.request("PATCH", "/session/transfers/nope", kSameOrigin);
  CHECK(r.status == 400);
  r = c.request("PATCH", "/session/transfers/" + good, kSameOrigin);
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/");
  CHECK(!cookie_pair(r, "session_token").empty());
  // The browser posts the form: Rack::MethodOverride turns the POST into a PUT.
  Client e(f.port());
  r = e.request("POST", "/session/transfers/" + good, kSameOrigin + kForm, "_method=put");
  CHECK(r.status == 302);
  CHECK(!cookie_pair(r, "session_token").empty());
  r = e.request("POST", "/session/transfers/" + good, kSameOrigin + kForm, "x=1");
  CHECK(r.status == 404);

  f.write([&](db::Tx& tx) -> Status {
    if (auto done = tx.conn().exec(kDeactivate, 1); !done) return std::unexpected(done.error());
    return {};
  });
  Client d(f.port());
  r = d.request("PUT", "/session/transfers/" + good, kSameOrigin);
  CHECK(r.status == 400);
}

TEST_CASE("allow_browser: an old browser gets the upgrade page") {
  Fixture f;
  Client c(f.port());
  Reply r = c.request("GET", "/session/new",
                      "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                      "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/100.0.0.0 Safari/537.36\r\n");
  REQUIRE(r.status == 200);
  CHECK(r.body.find("<title>Unsupported browser</title>") != std::string::npos);
  CHECK(r.body.find("Upgrade to a supported web browser") != std::string::npos);
  CHECK(r.body.find("<span> 17.2+</span>") != std::string::npos);
  CHECK(r.body.find("<strong>Opera</strong>") != std::string::npos);
  r = c.request("GET", "/session/new",
                "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_11_1) AppleWebKit/601.2.4 (KHTML, like Gecko) "
                "Version/9.0.1 Safari/601.2.4 facebookexternalhit/1.1 Facebot Twitterbot/1.0\r\n");
  CHECK(r.body.find("<title>Campfire</title>") != std::string::npos);
  // A current browser, a bot and no header get the page.
  r = c.request("GET", "/session/new", kChrome124);
  CHECK(r.body.find("<title>Sign in</title>") != std::string::npos);
  r = c.request("GET", "/session/new",
                "User-Agent: Mozilla/5.0 (compatible; Googlebot/2.1; +http://www.google.com/bot.html)\r\n");
  CHECK(r.body.find("<title>Sign in</title>") != std::string::npos);
  r = c.request("GET", "/session/new");
  CHECK(r.body.find("<title>Sign in</title>") != std::string::npos);
}

TEST_CASE("bans and deactivated users") {
  Fixture f;
  Client c(f.port());
  f.write([&](db::Tx& tx) -> Status {
    if (auto done = tx.conn().exec(kInsertBan, "203.0.113.9"); !done) return std::unexpected(done.error());
    return {};
  });
  // A banned address cannot send a write. A read still works.
  Reply r = c.request("POST", "/session", kSameOrigin + kForm + "X-Forwarded-For: 203.0.113.9\r\n",
                      "email_address=david%40example.com&password=" + std::string(kPassword));
  CHECK(r.status == 429);
  CHECK(r.header("content-type") == "text/html");
  r = c.request("GET", "/session/new", "X-Forwarded-For: 203.0.113.9\r\n");
  CHECK(r.status == 200);

  f.write([&](db::Tx& tx) -> Status {
    if (auto done = tx.conn().exec(kDeactivate, 1); !done) return std::unexpected(done.error());
    return {};
  });
  r = c.request("POST", "/session", kSameOrigin + kForm,
                "email_address=david%40example.com&password=" + std::string(kPassword));
  CHECK(r.status == 401);
}

}  // namespace campfire::app::testing
