// End to end tests of the room page, the messages page and the refresh. Rails: rooms_controller#show,
// messages_controller#index, rooms/refreshes_controller. Rust: crates/campfire/src/controllers/rooms.rs, messages.rs,
// rooms/refreshes.rs.
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

std::string sign_in(Client& c, const std::string& email) {
  const Reply r =
      c.request("POST", "/session", kSameOrigin + kForm, "email_address=" + email + "&password=" + kPassword);
  REQUIRE(r.status == 302);
  return "Cookie: " + cookie_pair(r, "session_token") + "\r\n";
}

void seed_room(Fixture& f) {
  f.write([&](db::Tx& tx) -> Status {
    return tx.conn().exec_sql(
        "INSERT INTO rooms (name, type, creator_id, created_at, updated_at) VALUES "
        "('Designers', 'Rooms::Open', 1, '2026-03-02 16:00:00', '2026-03-02 16:00:00');"
        "INSERT INTO memberships (room_id, user_id, involvement, connections, created_at, updated_at) VALUES "
        "(1, 1, 'everything', 0, '2026-03-02 16:00:00', '2026-03-02 16:00:00');");
  });
}

const std::string kTurbo = "Accept: text/vnd.turbo-stream.html, text/html, application/xhtml+xml\r\n";

void post_message(Client& c, const std::string& cookie, int n) {
  const Reply r = c.request("POST", "/rooms/1/messages", cookie + kSameOrigin + kForm + kTurbo,
                            "message%5Bbody%5D=%3Cp%3EHello+" + std::to_string(n) +
                                "%3C%2Fp%3E&message%5Bclient_message_id%5D=cm-" + std::to_string(n));
  REQUIRE(r.status == 200);
}

}  // namespace

TEST_CASE("rooms: the room page, the cookie and the page cache") {
  Fixture f;
  seed_room(f);
  Client c(f.port());
  const std::string david = sign_in(c, "david@example.com");
  post_message(c, david, 1);
  post_message(c, david, 2);

  Reply r = c.request("GET", "/rooms/1", david);
  REQUIRE(r.status == 200);
  CHECK(r.body.find("<div id=\"message-area\" class=\"message-area\"") != std::string::npos);
  CHECK(r.body.find("<div id=\"message_cm-1\" class=\"message \"") != std::string::npos);
  CHECK(r.body.find("<div id=\"message_cm-2\" class=\"message \"") != std::string::npos);
  CHECK(r.body.find("<meta name=\"current-room-id\" content=\"1\">") != std::string::npos);
  CHECK(r.body.find("channel=\"RoomMessagesChannel\"") != std::string::npos);
  CHECK(cookie_pair(r, "last_room") == "last_room=1");
  const std::string etag = r.header("etag");
  CHECK(etag.starts_with("W/\""));

  // The cookie is set only when it changes. The second page comes from the page cache with the same ETag.
  std::string with_room = david;
  with_room.insert(with_room.size() - 2, "; last_room=1");
  r = c.request("GET", "/rooms/1", with_room);
  REQUIRE(r.status == 200);
  CHECK(cookie_pair(r, "last_room").empty());
  CHECK(r.header("etag") == etag);
  r = c.request("GET", "/rooms/1", david + "If-None-Match: " + etag + "\r\n");
  CHECK(r.status == 304);
  r = c.request("HEAD", "/rooms/1", david);
  CHECK(r.status == 200);
  CHECK(r.header("etag") == etag);

  // A new message changes the page.
  post_message(c, david, 3);
  r = c.request("GET", "/rooms/1", david);
  CHECK(r.header("etag") != etag);
  CHECK(r.body.find("message_cm-3") != std::string::npos);

  // The room at a message, a room that is not there, a format with no template and no session.
  r = c.request("GET", "/rooms/1/@2", david);
  CHECK(r.status == 200);
  CHECK(r.body.find("message_cm-2") != std::string::npos);
  r = c.request("GET", "/rooms/99", david);
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/");
  r = c.request("GET", "/rooms/1", david + "Accept: application/json\r\n");
  CHECK(r.status == 406);
  r = c.request("GET", "/rooms/1");
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/session/new");
}

TEST_CASE("messages: the page of messages and the refresh") {
  Fixture f;
  seed_room(f);
  Client c(f.port());
  const std::string david = sign_in(c, "david@example.com");

  // No message: `head :no_content`.
  Reply r = c.request("GET", "/rooms/1/messages", david);
  CHECK(r.status == 204);
  for (int n = 1; n <= 3; ++n) post_message(c, david, n);
  // The test clock is frozen: give the messages the times of 3 minutes.
  f.write([&](db::Tx& tx) -> Status {
    return tx.conn().exec_sql("UPDATE messages SET created_at = '2026-03-02 16:0' || id || ':00'");
  });

  r = c.request("GET", "/rooms/1/messages", david);
  REQUIRE(r.status == 200);
  CHECK(r.body.find("message_cm-1") != std::string::npos);
  CHECK(r.body.find("<html") == std::string::npos);  // `layout false`
  CHECK(r.header("link").empty());
  const std::string etag = r.header("etag");
  CHECK(!etag.empty());
  r = c.request("GET", "/rooms/1/messages", david + "If-None-Match: " + etag + "\r\n");
  CHECK(r.status == 304);

  r = c.request("GET", "/rooms/1/messages?before=3", david);
  REQUIRE(r.status == 200);
  CHECK(r.body.find("message_cm-2") != std::string::npos);
  CHECK(r.body.find("message_cm-3") == std::string::npos);
  r = c.request("GET", "/rooms/1/messages?after=3", david);
  CHECK(r.status == 204);
  r = c.request("GET", "/rooms/1/messages?before=99", david);
  CHECK(r.status == 404);
  r = c.request("GET", "/rooms/9/messages", david);
  CHECK(r.status == 404);

  // The refresh: the messages created since a time, as an append.
  r = c.request("GET", "/rooms/1/refresh?since=0", david + kTurbo);
  REQUIRE(r.status == 200);
  CHECK(r.header("content-type") == "text/vnd.turbo-stream.html; charset=utf-8");
  CHECK(r.body.starts_with("<turbo-stream action=\"append\" target=\"messages_rooms_open_1\"><template>\n  "));
  r = c.request("GET", "/rooms/1/refresh?since=4102444800000", david + kTurbo);
  REQUIRE(r.status == 200);
  CHECK(r.body == "\n");
  r = c.request("GET", "/rooms/1/refresh?since=0", david);
  CHECK(r.status == 406);
}

// A request body that the worker reads in many steps moves the read buffer: the views of the request must stay valid
// (the sanitizer builds catch a view into the old buffer).
TEST_CASE("messages: a large body does not break the request") {
  Fixture f;
  seed_room(f);
  Client c(f.port());
  const std::string david = sign_in(c, "david@example.com");
  const std::string body =
      "message%5Bbody%5D=%3Cp%3E" + std::string(200000, 'x') + "%3C%2Fp%3E&message%5Bclient_message_id%5D=cm-big";
  const Reply r = c.request("POST", "/rooms/1/messages", david + kSameOrigin + kForm + kTurbo, body);
  CHECK(r.status == 200);
  CHECK(r.body.starts_with("<turbo-stream action=\"append\" target=\"messages_rooms_open_1\"><template>"));
}

}  // namespace campfire::app::testing
