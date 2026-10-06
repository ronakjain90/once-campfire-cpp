// End to end tests of the area A4: accounts, users, profiles, avatars, bots, bans, push subscriptions, QR codes.
// Rails: accounts, users, users/*, qr_code controllers. Rust: crates/campfire/src/controllers/{accounts*,users*,qr_code*}.
#include <doctest.h>

#include "app/tests/fixture.hpp"
#include "compat/base64.hpp"
#include "compat/signed_id.hpp"
#include "models/hooks.hpp"

namespace campfire::app::testing {

namespace {

std::string cookie_pair(const Reply& reply, const std::string& name) {
  for (const auto& [k, v] : reply.headers) {
    if (k == "set-cookie" && v.starts_with(name + "=")) return v.substr(0, v.find(';'));
  }
  return {};
}

// The cookie of a signed in user.
std::string sign_in(Client& c, const std::string& email) {
  const Reply r = c.request("POST", "/session", kSameOrigin + kForm, "email_address=" + email + "&password=" + kPassword);
  REQUIRE(r.status == 302);
  return "Cookie: " + cookie_pair(r, "session_token") + "\r\n";
}

const db::Query<void(std::string_view, std::string_view, std::string_view, std::int64_t)> kAddUser{
    "INSERT INTO users (name, email_address, password_digest, role, status, created_at, updated_at) VALUES "
    "(?, ?, ?, ?, 0, '2026-03-02 16:00:00', '2026-03-02 16:00:00')"};

void add_user(Fixture& f, const std::string& name, const std::string& email, std::int64_t role) {
  const std::string digest = req::bcrypt::hash_password(kPassword, req::bcrypt::kMinCost);
  f.write([&](db::Tx& tx) -> Status {
    if (auto r = tx.conn().exec(kAddUser, name, email, digest, role); !r) return std::unexpected(r.error());
    return {};
  });
}

std::string avatar_path(Fixture& f, std::int64_t user_id) {
  const std::string token =
      compat::signed_id::generate(f.state->secrets, "User", user_id, "avatar", std::nullopt);
  return "/users/" + token + "/avatar";
}

}  // namespace

TEST_CASE("avatar: the initials, the cache and a change of the name") {
  Fixture f;
  Client c(f.port());
  const std::string cookie = sign_in(c, "david@example.com");
  const std::string path = avatar_path(f, 1);
  Reply r = c.request("GET", path, cookie);
  REQUIRE(r.status == 200);
  CHECK(r.header("content-type") == "image/svg+xml; charset=utf-8");
  CHECK(r.header("cache-control") == "max-age=1800, public, stale-while-revalidate=604800");
  CHECK(r.header("etag").starts_with("W/\""));
  const std::string etag = r.header("etag");
  const std::string svg = r.body.find("<svg") == std::string::npos ? gunzip(r.body) : r.body;
  CHECK(svg.find(">\n      D\n    </text>") != std::string::npos);
  CHECK(svg.find("textLength") == std::string::npos);  // fewer than 3 initials

  // The same request again is the same (the worker cache), and a conditional GET is a 304.
  r = c.request("GET", path, cookie);
  CHECK(r.header("etag") == etag);
  r = c.request("GET", path, cookie + "If-None-Match: " + etag + "\r\n");
  CHECK(r.status == 304);

  // A new name changes the initials and the validator: the cache drops the entry.
  r = c.request("PATCH", "/users/me/profile", cookie + kSameOrigin + kForm, "user[name]=Ann+Marie+Smith");
  CHECK(r.status == 302);
  // The front cache keeps the old response for its path and query (30 minutes): the `v` of the URL changes it.
  r = c.request("GET", path + "?v=2", cookie);
  REQUIRE(r.status == 200);
  const std::string after = r.body.find("<svg") == std::string::npos ? gunzip(r.body) : r.body;
  CHECK(after.find(">\n      AMS\n    </text>") != std::string::npos);
  CHECK(r.header("etag") == etag);  // the clock is frozen, so `updated_at` is the same: the cache entry is new anyway
  CHECK(after.find("textLength=\"85%\"") != std::string::npos);  // three initials

  // A bad signature is `head :not_found`. A good signature for a user that is not there is a 404 page.
  r = c.request("GET", "/users/bogus/avatar", cookie);
  CHECK(r.status == 404);
  r = c.request("GET", avatar_path(f, 999), cookie);
  CHECK(r.status == 404);
}

TEST_CASE("avatar: a bot gets the default avatar") {
  Fixture f;
  add_user(f, "Bender", "bender@example.com", 2);
  Client c(f.port());
  const std::string cookie = sign_in(c, "david@example.com");
  Reply r = c.request("GET", avatar_path(f, 2), cookie);
  REQUIRE(r.status == 200);
  CHECK(r.header("content-type") == "image/svg+xml");
  CHECK(r.header("content-disposition").starts_with("inline; filename=\"default-bot-avatar.svg\""));
}

TEST_CASE("qr code: the SVG, the cache headers and a bad id") {
  Fixture f;
  Client c(f.port());
  const std::string id = compat::base64::urlsafe_encode_padded("http://example.com");
  Reply r = c.request("GET", "/qr_code/" + id);
  REQUIRE(r.status == 200);
  CHECK(r.header("content-type") == "image/svg+xml; charset=utf-8");
  CHECK(r.header("cache-control") == "max-age=31556952, public");
  const std::string body = r.body.starts_with("<?xml") ? r.body : gunzip(r.body);
  CHECK(body.starts_with("<?xml version=\"1.0\" standalone=\"yes\"?><svg version=\"1.1\""));
  r = c.request("GET", "/qr_code/a");  // not Base64: ArgumentError
  CHECK(r.status == 500);
  r = c.request("GET", "/qr_code/" + compat::base64::urlsafe_encode_padded(std::string(3000, 'a')));
  CHECK(r.status == 422);
}

TEST_CASE("profile: update the name, the email address, the password and the bio") {
  Fixture f;
  Client c(f.port());
  const std::string cookie = sign_in(c, "david@example.com");
  Reply r = c.request("GET", "/users/me/profile", cookie);
  REQUIRE(r.status == 200);
  CHECK(r.body.find("<title>David</title>") != std::string::npos);
  CHECK(r.body.find("Log out") != std::string::npos);
  r = c.request("PATCH", "/users/me/profile", cookie + kSameOrigin + kForm,
                "user[name]=Dave&user[bio]=Hi&user[email_address]=dave%40example.com&user[password]=newsecret12345");
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/users/me/profile");
  r = c.request("GET", "/users/me/profile", cookie);
  CHECK(r.body.find("<title>Dave</title>") != std::string::npos);
  CHECK(r.body.find("A few words about yourself") != std::string::npos);
  CHECK(r.body.find(">\nHi</textarea>") != std::string::npos);
  // The new password signs in.
  r = c.request("POST", "/session", kSameOrigin + kForm, "email_address=dave%40example.com&password=newsecret12345");
  CHECK(r.status == 302);
  CHECK(!cookie_pair(r, "session_token").empty());
  // A missing `user` is a 400.
  r = c.request("PATCH", "/users/me/profile", cookie + kSameOrigin + kForm, "x=1");
  CHECK(r.status == 400);
  // The routes that Rails has, but the controller does not.
  r = c.request("GET", "/users/me/profile/edit", cookie);
  CHECK(r.status == 404);
}

TEST_CASE("account: settings, the join code, the people list and the roles") {
  Fixture f;
  add_user(f, "Kevin", "kevin@example.com", 0);
  Client c(f.port());
  const std::string cookie = sign_in(c, "david@example.com");
  Reply r = c.request("GET", "/account/edit", cookie);
  REQUIRE(r.status == 200);
  CHECK(r.body.find("value=\"http://test.example/join/abcd-efgh-ijkl\"") != std::string::npos);
  CHECK(r.body.find("action=\"/account.1\"") != std::string::npos);
  CHECK(r.body.find("<strong>Kevin</strong>") != std::string::npos);

  r = c.request("PATCH", "/account", cookie + kSameOrigin + kForm,
                "account[name]=Renamed&account[settings][restrict_room_creation_to_administrators]=true");
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/account/edit");
  r = c.request("GET", "/account/edit", cookie);
  CHECK(r.body.find("value=\"Renamed\"") != std::string::npos);
  CHECK(r.body.find("checked") != std::string::npos);
  // A setting that the schema does not know raises.
  r = c.request("PATCH", "/account", cookie + kSameOrigin + kForm, "account[settings][nope]=1");
  CHECK(r.status == 500);

  r = c.request("POST", "/account/join_code", cookie + kSameOrigin);
  CHECK(r.status == 302);
  r = c.request("GET", "/account/edit", cookie);
  CHECK(r.body.find("abcd-efgh-ijkl") == std::string::npos);

  // The people list: the turbo stream only.
  r = c.request("GET", "/account/users?format=turbo_stream", cookie);
  REQUIRE(r.status == 200);
  CHECK(r.header("content-type") == "text/vnd.turbo-stream.html; charset=utf-8");
  CHECK(r.body.find("<turbo-stream action=\"replace\" target=\"next_page_container\">") != std::string::npos);
  r = c.request("GET", "/account/users", cookie);
  CHECK(r.status == 406);

  r = c.request("PATCH", "/account/users/2", cookie + kSameOrigin + kForm, "user[role]=administrator");
  CHECK(r.status == 302);
  r = c.request("GET", "/account/edit", cookie);
  CHECK(r.body.find("Role: Administrator") != std::string::npos);
  r = c.request("DELETE", "/account/users/2", cookie + kSameOrigin);
  CHECK(r.status == 302);
  r = c.request("GET", "/account/edit", cookie);
  CHECK(r.body.find("<strong>Kevin</strong>") == std::string::npos);  // deactivated users are not listed
  r = c.request("DELETE", "/account/users/2", cookie + kSameOrigin);
  CHECK(r.status == 404);  // not active any more
}

TEST_CASE("account: a member cannot change it") {
  Fixture f;
  add_user(f, "Kevin", "kevin@example.com", 0);
  Client c(f.port());
  const std::string cookie = sign_in(c, "kevin@example.com");
  Reply r = c.request("PATCH", "/account", cookie + kSameOrigin + kForm, "account[name]=x");
  CHECK(r.status == 403);
  r = c.request("POST", "/account/join_code", cookie + kSameOrigin);
  CHECK(r.status == 403);
  r = c.request("GET", "/account/bots", cookie);
  CHECK(r.status == 403);
  r = c.request("GET", "/account/edit", cookie);
  REQUIRE(r.status == 200);  // a member sees the page, without the forms
  CHECK(r.body.find("action=\"/account.1\"") == std::string::npos);
}

TEST_CASE("bans: a user without a session, and the validation of the addresses") {
  Fixture f;
  add_user(f, "Kevin", "kevin@example.com", 0);
  add_user(f, "Rita", "rita@example.com", 0);
  Client c(f.port());
  const std::string cookie = sign_in(c, "david@example.com");
  Reply r = c.request("POST", "/users/2/ban", cookie + kSameOrigin);
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/users/2");
  r = c.request("GET", "/users/2", cookie);
  REQUIRE(r.status == 200);
  CHECK(r.body.find("Remove ban") != std::string::npos);
  r = c.request("DELETE", "/users/2/ban", cookie + kSameOrigin);
  CHECK(r.status == 302);
  r = c.request("GET", "/users/2", cookie);
  CHECK(r.body.find("<span>Ban Kevin</span>") != std::string::npos);

  // Rita signs in from the loopback address: the ban row is invalid (`ip_address_is_public`), as in Rails.
  Client d(f.port());
  const std::string rita = sign_in(d, "rita@example.com");
  CHECK(!rita.empty());
  r = c.request("POST", "/users/3/ban", cookie + kSameOrigin);
  CHECK(r.status == 500);
  r = c.request("GET", "/users/3", cookie);
  CHECK(r.body.find("<span>Ban Rita</span>") != std::string::npos);  // the write rolled back
  r = c.request("POST", "/users/999/ban", cookie + kSameOrigin);
  CHECK(r.status == 404);
}

TEST_CASE("bots: create, edit, new key and remove") {
  Fixture f;
  Client c(f.port());
  const std::string cookie = sign_in(c, "david@example.com");
  Reply r = c.request("GET", "/account/bots/new", cookie);
  REQUIRE(r.status == 200);
  r = c.request("POST", "/account/bots", cookie + kSameOrigin + kForm,
                "user[name]=Robo&user[webhook_url]=http%3A%2F%2F127.0.0.1%3A9%2Fhook");
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/account/bots");
  r = c.request("GET", "/account/bots", cookie);
  REQUIRE(r.status == 200);
  CHECK(r.body.find("<strong>Robo</strong>") != std::string::npos);
  r = c.request("GET", "/account/bots/2/edit", cookie);
  REQUIRE(r.status == 200);
  CHECK(r.body.find("value=\"http://127.0.0.1:9/hook\"") != std::string::npos);
  r = c.request("PATCH", "/account/bots/2", cookie + kSameOrigin + kForm, "user[name]=Robo+2&user[webhook_url]=");
  CHECK(r.status == 302);
  r = c.request("GET", "/account/bots/2/edit", cookie);
  CHECK(r.body.find("value=\"Robo 2\"") != std::string::npos);
  CHECK(r.body.find("http://127.0.0.1:9/hook") == std::string::npos);  // a blank webhook URL removes the webhook
  r = c.request("PUT", "/account/bots/2/key", cookie + kSameOrigin);
  CHECK(r.status == 302);
  r = c.request("DELETE", "/account/bots/2", cookie + kSameOrigin);
  CHECK(r.status == 302);
  r = c.request("GET", "/account/bots/2/edit", cookie);
  CHECK(r.status == 404);
  r = c.request("POST", "/account/bots", cookie + kSameOrigin + kForm, "user[webhook_url]=x");
  CHECK(r.status == 500);  // users.name is NOT NULL
}

TEST_CASE("push subscriptions: validation, the list and the test notification") {
  Fixture f;
  Client c(f.port());
  const std::string cookie = sign_in(c, "david@example.com");
  Reply r = c.request("GET", "/users/me/push_subscriptions", cookie);
  REQUIRE(r.status == 200);
  // An endpoint that is not a permitted push service is a 422.
  r = c.request("POST", "/users/me/push_subscriptions", cookie + kSameOrigin + kForm,
                "push_subscription[endpoint]=https%3A%2F%2Fpush.example.com%2Fx&push_subscription[p256dh_key]=k&"
                "push_subscription[auth_key]=a");
  CHECK(r.status == 422);
  r = c.request("POST", "/users/me/push_subscriptions", cookie + kSameOrigin + "Content-Type: application/json\r\n",
                "{\"endpoint\":\"http://fcm.googleapis.com/x\",\"p256dh_key\":\"k\",\"auth_key\":\"a\"}");
  CHECK(r.status == 422);  // wrapped parameters, and not https
  r = c.request("POST", "/users/me/push_subscriptions", cookie + kSameOrigin + kForm, "x=1");
  CHECK(r.status == 400);
  r = c.request("POST", "/users/me/push_subscriptions/99/test_notifications", cookie + kSameOrigin);
  CHECK(r.status == 404);
  r = c.request("DELETE", "/users/me/push_subscriptions/99", cookie + kSameOrigin);
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/users/me/push_subscriptions");
}

TEST_CASE("users: the page of a user, and a missing one") {
  Fixture f;
  add_user(f, "Kevin", "kevin@example.com", 0);
  Client c(f.port());
  const std::string cookie = sign_in(c, "david@example.com");
  Reply r = c.request("GET", "/users/2", cookie);
  REQUIRE(r.status == 200);
  CHECK(r.body.find("<title>Kevin</title>") != std::string::npos);
  CHECK(r.body.find("mailto:kevin@example.com") != std::string::npos);  // an administrator sees the address
  CHECK(r.body.find("session_transfer_url") != std::string::npos);
  r = c.request("GET", "/users/1", cookie);
  CHECK(r.body.find("Edit my profile") != std::string::npos);
  r = c.request("GET", "/users/999", cookie);
  CHECK(r.status == 404);
  Client d(f.port());
  const std::string kevin = sign_in(d, "kevin@example.com");
  r = d.request("GET", "/users/1", kevin);
  REQUIRE(r.status == 200);
  CHECK(r.body.find("mailto:") == std::string::npos);  // a member does not
  CHECK(r.body.find("session_transfer_url") == std::string::npos);
}

}  // namespace campfire::app::testing
