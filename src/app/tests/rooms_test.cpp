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
