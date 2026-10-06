// End to end tests of the room redirects. Rails: RoomsController, Rooms::OpensController, Rooms::ClosedsController.
#include <doctest.h>

#include "app/tests/fixture.hpp"

namespace campfire::app::testing {

namespace {

const db::Query<void(std::string_view, std::string_view)> kInsertRoom{
    "INSERT INTO rooms (name, type, creator_id, created_at, updated_at) VALUES "
    "(?, ?, 1, '2026-03-02 16:00:00', '2026-03-02 16:00:00')"};
const db::Query<void(std::int64_t)> kInsertMembership{
    "INSERT INTO memberships (room_id, user_id, involvement, created_at, updated_at) VALUES "
    "(?, 1, 'mentions', '2026-03-02 16:00:00', '2026-03-02 16:00:00')"};

void add_room(Fixture& f, std::string_view name, std::string_view type, bool member) {
  QueueScheduler scheduler;
  auto wrote = db::testing::run_task(scheduler, f.state->db->write(scheduler, [&](db::Tx& tx) -> Status {
    if (auto r = tx.conn().exec(kInsertRoom, name, type); !r) return std::unexpected(r.error());
    if (member) {
      if (auto r = tx.conn().exec(kInsertMembership, tx.conn().last_insert_rowid()); !r) {
        return std::unexpected(r.error());
      }
    }
    return {};
  }));
  REQUIRE(wrote.has_value());
}

std::string sign_in(Client& c) {
  const Reply r = c.request("POST", "/session", kSameOrigin + kForm,
                            std::string("email_address=david@example.com&password=") + kPassword);
  REQUIRE(r.status == 302);
  for (const auto& [k, v] : r.headers) {
    if (k == "set-cookie" && v.starts_with("session_token=")) return v.substr(0, v.find(';'));
  }
  FAIL("no session cookie");
  return {};
}

}  // namespace

TEST_CASE("rooms: index redirects to the last room of the user") {
  Fixture f;
  add_room(f, "Open", "Rooms::Open", true);
  add_room(f, "Hidden", "Rooms::Closed", false);
  add_room(f, "Closed", "Rooms::Closed", true);
  Client c(f.port());
  const std::string token = sign_in(c);
  const Reply r = c.request("GET", "/rooms", "Cookie: " + token + "\r\n");
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/rooms/3");
}

TEST_CASE("rooms: opens and closeds show redirect and remember the room") {
  Fixture f;
  add_room(f, "Open", "Rooms::Open", true);
  add_room(f, "Direct", "Rooms::Direct", true);
  add_room(f, "Hidden", "Rooms::Closed", false);
  Client c(f.port());
  const std::string token = sign_in(c);
  const std::string cookie = "Cookie: " + token + "\r\n";

  Reply r = c.request("GET", "/rooms/opens/1", cookie);
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/rooms/1");
  CHECK(r.header("set-cookie").starts_with("last_room=1;"));

  r = c.request("GET", "/rooms/closeds/1x", cookie);
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/rooms/1");

  // A direct room is out of reach of the open and closed controllers, as is a room without a membership.
  for (const char* path : {"/rooms/opens/2", "/rooms/closeds/3", "/rooms/opens/abc", "/rooms/closeds/99"}) {
    r = c.request("GET", path, cookie);
    CHECK(r.status == 302);
    CHECK(r.header("location") == "http://test.example/");
  }
}

}  // namespace campfire::app::testing

namespace campfire::app::testing {

TEST_CASE("sidebar: rooms, direct rooms and placeholders") {
  Fixture f;
  add_room(f, "Zebra", "Rooms::Open", true);
  add_room(f, "alpha", "Rooms::Closed", true);
  add_room(f, "Hidden", "Rooms::Closed", false);
  Client c(f.port());
  const std::string token = sign_in(c);
  const std::string cookie = "Cookie: " + token + "\r\n";

  // A Turbo-Frame request gets the frame layout; the rooms are in the order of `LOWER(name)`.
  Reply r = c.request("GET", "/users/me/sidebar", cookie + "Turbo-Frame: user_sidebar\r\n");
  REQUIRE(r.status == 200);
  CHECK(r.body.find("<turbo-frame data-turbo-permanent=\"true\"") != std::string::npos);
  const auto alpha = r.body.find("data-sorted-list-name=\"alpha\"");
  const auto zebra = r.body.find("data-sorted-list-name=\"Zebra\"");
  REQUIRE(alpha != std::string::npos);
  REQUIRE(zebra != std::string::npos);
  CHECK(alpha < zebra);
  CHECK(r.body.find("Hidden") == std::string::npos);
  CHECK(r.body.find("href=\"/rooms/opens/new\"") != std::string::npos);
  CHECK(r.body.find("<!DOCTYPE html>") == std::string::npos);

  // Without the frame header the sidebar is a full page.
  r = c.request("GET", "/users/me/sidebar", cookie);
  CHECK(r.status == 200);
  CHECK(r.body.starts_with("<!DOCTYPE html>"));
}

}  // namespace campfire::app::testing

namespace campfire::app::testing {

namespace {

// A write transaction that reads one number: the tests count rows with it.
std::int64_t scalar(Fixture& f, std::string_view sql) {
  const db::Query<std::int64_t()> query{sql};
  std::int64_t value = -1;
  QueueScheduler scheduler;
  auto done = db::testing::run_task(scheduler, f.state->db->write(scheduler, [&](db::Tx& tx) -> Status {
    Arena arena(256);
    auto row = tx.conn().first(query, arena);
    if (!row) return std::unexpected(row.error());
    if (*row) value = **row;
    return {};
  }));
  REQUIRE(done.has_value());
  return value;
}

void add_member(Fixture& f, std::string_view name, std::string_view email, std::int64_t role) {
  QueueScheduler scheduler;
  const std::string digest = req::bcrypt::hash_password(kPassword, req::bcrypt::kMinCost);
  auto wrote = db::testing::run_task(scheduler, f.state->db->write(scheduler, [&](db::Tx& tx) -> Status {
    if (auto r = tx.conn().exec(kInsertUser, name, email, digest, role); !r) return std::unexpected(r.error());
    return {};
  }));
  REQUIRE(wrote.has_value());
}

std::string sign_in_as(Client& c, const std::string& email) {
  const Reply r =
      c.request("POST", "/session", kSameOrigin + kForm, "email_address=" + email + "&password=" + kPassword);
  REQUIRE(r.status == 302);
  for (const auto& [k, v] : r.headers) {
    if (k == "set-cookie" && v.starts_with("session_token=")) return v.substr(0, v.find(';'));
  }
  FAIL("no session cookie");
  return {};
}

struct Sent {
  std::string stream;
  std::string html;
};

}  // namespace

TEST_CASE("rooms: open room forms, create, update and the broadcasts") {
  Fixture f;
  add_member(f, "Jason", "jason@example.com", 0);
  std::vector<Sent> sent;
  f.state->turbo_broadcast = [&](std::string_view stream, std::string_view html) {
    sent.push_back({std::string(stream), std::string(html)});
  };
  Client c(f.port());
  const std::string cookie = "Cookie: " + sign_in(c) + "\r\n";

  Reply r = c.request("GET", "/rooms/opens/new", cookie);
  REQUIRE(r.status == 200);
  CHECK(r.body.find("<title>New chat room</title>") != std::string::npos);
  CHECK(r.body.find("value=\"New room\"") != std::string::npos);
  CHECK(r.body.find("action=\"/rooms/opens\"") != std::string::npos);
  CHECK(r.body.find("data-value=\"jason\"") != std::string::npos);

  // Creating a room grants it to every active user and prepends it to everyone's list.
  r = c.request("POST", "/rooms/opens", kSameOrigin + kForm + cookie, "room%5Bname%5D=Lounge");
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/rooms/1");
  CHECK(scalar(f, "SELECT COUNT(*) FROM memberships WHERE room_id = 1") == 2);
  REQUIRE(sent.size() == 1);
  CHECK(sent[0].stream == "rooms");
  CHECK(sent[0].html.starts_with("<turbo-stream action=\"prepend\" target=\"shared_rooms\"><template>"));
  CHECK(sent[0].html.find("data-sorted-list-name=\"Lounge\"") != std::string::npos);

  // A missing `room` param is a 400.
  r = c.request("POST", "/rooms/opens", kSameOrigin + kForm + cookie, "x=1");
  CHECK(r.status == 400);

  r = c.request("GET", "/rooms/opens/1/edit", cookie);
  REQUIRE(r.status == 200);
  CHECK(r.body.find("<title>Edit settings for Lounge</title>") != std::string::npos);
  CHECK(r.body.find("name=\"_method\" value=\"patch\"") != std::string::npos);
  CHECK(r.body.find("Delete Lounge") != std::string::npos);

  sent.clear();
  r = c.request("PATCH", "/rooms/opens/1", kSameOrigin + kForm + cookie, "room%5Bname%5D=Den");
  CHECK(r.status == 302);
  REQUIRE(sent.size() == 1);
  CHECK(sent[0].html.starts_with("<turbo-stream action=\"replace\" target=\"list_rooms_open_1\">"));
  CHECK(scalar(f, "SELECT COUNT(*) FROM rooms WHERE name = 'Den'") == 1);

  // Destroying removes the room, its memberships, and tells everyone.
  sent.clear();
  r = c.request("DELETE", "/rooms/1", kSameOrigin + cookie);
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/");
  CHECK(scalar(f, "SELECT COUNT(*) FROM rooms") == 0);
  CHECK(scalar(f, "SELECT COUNT(*) FROM memberships") == 0);
  REQUIRE(sent.size() == 1);
  CHECK(sent[0].html == "<turbo-stream action=\"remove\" target=\"list_rooms_open_1\"></turbo-stream>");
}

TEST_CASE("rooms: only the creator or an administrator changes a room, and the account can limit creation") {
  Fixture f;
  add_member(f, "Jason", "jason@example.com", 0);
  Client admin(f.port());
  const std::string admin_cookie = "Cookie: " + sign_in(admin) + "\r\n";
  Reply r = admin.request("POST", "/rooms/opens", kSameOrigin + kForm + admin_cookie, "room%5Bname%5D=Lounge");
  REQUIRE(r.status == 302);

  Client member(f.port());
  const std::string member_cookie = "Cookie: " + sign_in_as(member, "jason@example.com") + "\r\n";
  r = member.request("PATCH", "/rooms/opens/1", kSameOrigin + kForm + member_cookie, "room%5Bname%5D=Mine");
  CHECK(r.status == 403);
  r = member.request("DELETE", "/rooms/1", kSameOrigin + member_cookie);
  CHECK(r.status == 403);
  // The edit page still opens, but without the name field and the save button.
  r = member.request("GET", "/rooms/opens/1/edit", member_cookie);
  REQUIRE(r.status == 200);
  CHECK(r.body.find("name=\"room[name]\"") == std::string::npos);
  CHECK(r.body.find("<h1 class=\"flex-item-grow txt-x-large\">") != std::string::npos);

  QueueScheduler scheduler;
  auto limited = db::testing::run_task(scheduler, f.state->db->write(scheduler, [&](db::Tx& tx) -> Status {
    const db::Query<void()> q{"UPDATE accounts SET settings = '{\"restrict_room_creation_to_administrators\":true}'"};
    if (auto done = tx.conn().exec(q); !done) return std::unexpected(done.error());
    return {};
  }));
  REQUIRE(limited.has_value());
  r = member.request("GET", "/rooms/opens/new", member_cookie);
  CHECK(r.status == 403);
  r = member.request("POST", "/rooms/closeds", kSameOrigin + kForm + member_cookie, "room%5Bname%5D=X");
  CHECK(r.status == 403);
  r = admin.request("GET", "/rooms/closeds/new", admin_cookie);
  CHECK(r.status == 200);
}

TEST_CASE("rooms: closed rooms grant and revoke members") {
  Fixture f;
  add_member(f, "Jason", "jason@example.com", 0);
  add_member(f, "Kevin", "kevin@example.com", 0);
  std::vector<Sent> sent;
  f.state->turbo_broadcast = [&](std::string_view stream, std::string_view html) {
    sent.push_back({std::string(stream), std::string(html)});
  };
  Client c(f.port());
  const std::string cookie = "Cookie: " + sign_in(c) + "\r\n";

  Reply r = c.request("GET", "/rooms/closeds/new", cookie);
  REQUIRE(r.status == 200);
  // The creator is a fixed member: a hidden field and no switch.
  CHECK(r.body.find("<input type=\"hidden\" name=\"user_ids[]\" value=\"1\" />") != std::string::npos);

  r = c.request("POST", "/rooms/closeds", kSameOrigin + kForm + cookie,
                "room%5Bname%5D=Secret&user_ids%5B%5D=1&user_ids%5B%5D=2");
  CHECK(r.status == 302);
  CHECK(scalar(f, "SELECT COUNT(*) FROM memberships WHERE room_id = 1") == 2);
  // One broadcast for each member, to the member's own stream.
  REQUIRE(sent.size() == 2);
  CHECK(sent[0].stream != sent[1].stream);
  CHECK(sent[0].html.starts_with("<turbo-stream action=\"prepend\" target=\"shared_rooms\">"));

  r = c.request("GET", "/rooms/closeds/1/edit", cookie);
  REQUIRE(r.status == 200);
  CHECK(r.body.find("value=\"2\" class=\"switch__input\" checked=\"checked\"") != std::string::npos);
  CHECK(r.body.find("value=\"3\" class=\"switch__input\" />") != std::string::npos);

  // Jason goes, Kevin comes.
  sent.clear();
  r = c.request("PATCH", "/rooms/closeds/1", kSameOrigin + kForm + cookie,
                "room%5Bname%5D=Secret&user_ids%5B%5D=1&user_ids%5B%5D=3");
  CHECK(r.status == 302);
  CHECK(scalar(f, "SELECT COUNT(*) FROM memberships WHERE room_id = 1 AND user_id = 2") == 0);
  CHECK(scalar(f, "SELECT COUNT(*) FROM memberships WHERE room_id = 1 AND user_id = 3") == 1);
  CHECK(sent.size() == 2);
  CHECK(sent[0].html.starts_with("<turbo-stream action=\"replace\" target=\"list_rooms_closed_1\">"));
}

TEST_CASE("rooms: direct rooms are found again for the same people") {
  Fixture f;
  add_member(f, "Jason Fried", "jason@example.com", 0);
  std::vector<Sent> sent;
  f.state->turbo_broadcast = [&](std::string_view stream, std::string_view html) {
    sent.push_back({std::string(stream), std::string(html)});
  };
  Client c(f.port());
  const std::string cookie = "Cookie: " + sign_in(c) + "\r\n";

  Reply r = c.request("GET", "/rooms/directs/new", cookie);
  REQUIRE(r.status == 200);
  CHECK(r.body.find("<turbo-frame id=\"direct_rooms_control\" target=\"_top\">") != std::string::npos);

  r = c.request("POST", "/rooms/directs", kSameOrigin + kForm + cookie, "user_ids%5B%5D=2");
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/rooms/1");
  CHECK(scalar(f, "SELECT COUNT(*) FROM memberships WHERE room_id = 1 AND involvement = 'everything'") == 2);
  REQUIRE(sent.size() == 2);
  CHECK(sent[0].html.starts_with("<turbo-stream action=\"prepend\" target=\"direct_rooms\">"));
  CHECK(sent[0].html.find("id=\"list_rooms_direct_1\"") != std::string::npos);

  // The same people give the same room, and the user alone gets a room of one.
  r = c.request("POST", "/rooms/directs", kSameOrigin + kForm + cookie, "user_ids%5B%5D=2&user_ids%5B%5D=1");
  CHECK(r.header("location") == "http://test.example/rooms/1");
  CHECK(scalar(f, "SELECT COUNT(*) FROM rooms") == 1);
  r = c.request("POST", "/rooms/directs", kSameOrigin + kForm + cookie, "x=1");
  CHECK(r.header("location") == "http://test.example/rooms/2");

  r = c.request("GET", "/rooms/directs/1/edit", cookie);
  REQUIRE(r.status == 200);
  CHECK(r.body.find("<title>Edit settings for Jason Fried</title>") != std::string::npos);
  CHECK(r.body.find("<strong>Jason Fried</strong>") != std::string::npos);
  CHECK(r.body.find("<strong>David</strong>") == std::string::npos);

  // An open room is out of reach of the direct controller.
  r = c.request("DELETE", "/rooms/directs/1", kSameOrigin + cookie);
  CHECK(r.status == 302);
  CHECK(scalar(f, "SELECT COUNT(*) FROM rooms WHERE id = 1") == 0);
  r = c.request("GET", "/rooms/directs/99/edit", cookie);
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/");
}

TEST_CASE("rooms: involvement") {
  Fixture f;
  add_room(f, "Open", "Rooms::Open", true);
  std::vector<Sent> sent;
  f.state->turbo_broadcast = [&](std::string_view stream, std::string_view html) {
    sent.push_back({std::string(stream), std::string(html)});
  };
  Client c(f.port());
  const std::string cookie = "Cookie: " + sign_in(c) + "\r\n";

  Reply r = c.request("GET", "/rooms/1/involvement", cookie);
  REQUIRE(r.status == 200);
  CHECK(r.body.find("id=\"involvement_rooms_open_1\"") != std::string::npos);
  CHECK(r.body.find("Notifying about @ mentions") != std::string::npos);
  CHECK(r.body.find("/rooms/1/involvement?involvement=everything") != std::string::npos);

  r = c.request("PUT", "/rooms/1/involvement?involvement=invisible", kSameOrigin + cookie);
  CHECK(r.status == 302);
  CHECK(r.header("location") == "http://test.example/rooms/1/involvement");
  REQUIRE(sent.size() == 1);
  CHECK(sent[0].html == "<turbo-stream action=\"remove\" target=\"list_rooms_open_1\"></turbo-stream>");

  // Back from invisible puts the room in the sidebar again.
  sent.clear();
  r = c.request("PUT", "/rooms/1/involvement?involvement=mentions", kSameOrigin + cookie);
  CHECK(r.status == 302);
  REQUIRE(sent.size() == 1);
  CHECK(sent[0].html.starts_with("<turbo-stream action=\"prepend\" target=\"shared_rooms\">"));

  r = c.request("PUT", "/rooms/1/involvement?involvement=loud", kSameOrigin + cookie);
  CHECK(r.status == 500);
  r = c.request("GET", "/rooms/9/involvement", cookie);
  CHECK(r.status == 404);
}

}  // namespace campfire::app::testing
