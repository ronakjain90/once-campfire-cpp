// End to end tests of the remote disconnects. Rails: Membership after_destroy_commit, Authentication#terminate_session.
// Rust: crates/db/src/events.rs (Event::DisconnectUser).
#include <doctest.h>

#include "app/channels/server.hpp"
#include "app/tests/fixture.hpp"
#include "cable/protocol.hpp"
#include "models/hooks.hpp"
#include "models/room.hpp"

namespace campfire::app::testing {

namespace {

using channels::CableServer;
namespace protocol = campfire::cable::protocol;

struct Listener final : cable::Sink {
  void deliver(const cable::FramePtr& frame) override { frames.emplace_back(frame->text()); }
  std::vector<std::string> frames;
};

const db::Query<void()> kInsertRoom{
    "INSERT INTO rooms (name, type, creator_id, created_at, updated_at) VALUES "
    "('Open', 'Rooms::Open', 1, '2026-03-02 16:00:00', '2026-03-02 16:00:00')"};
const db::Query<void()> kInsertMembership{
    "INSERT INTO memberships (room_id, user_id, involvement, created_at, updated_at) VALUES "
    "(1, 1, 'mentions', '2026-03-02 16:00:00', '2026-03-02 16:00:00')"};

struct Cable {
  explicit Cable(Fixture& f) : server(*f.state, 1, [](unsigned) {}) {
    models::hooks::set_disconnect_user(
        [this](std::int64_t user_id, bool reconnect) { server.disconnect_user(user_id, reconnect); });
    server.hub().subscribe(0, protocol::internal_channel(channels::connection_identifier(1)), {}, &listener);
  }
  ~Cable() { models::hooks::set_disconnect_user(nullptr); }
  CableServer server;
  Listener listener;
};

}  // namespace

TEST_CASE("cable: revoking a membership disconnects the user with reconnect") {
  Fixture f;
  Cable cable(f);
  f.write([](db::Tx& tx) -> Status {
    if (auto r = tx.conn().exec(kInsertRoom); !r) return std::unexpected(r.error());
    if (auto r = tx.conn().exec(kInsertMembership); !r) return std::unexpected(r.error());
    return {};
  });
  models::Room room;
  room.id = 1;
  const std::int64_t revoked[] = {1};
  f.write([&](db::Tx& tx) -> Status { return models::rooms::revise(tx, room, {}, revoked); });
  cable.server.hub().drain(0);
  REQUIRE(cable.listener.frames.size() == 1);
  CHECK(cable.listener.frames[0] == protocol::remote_disconnect_payload(true));
}

TEST_CASE("cable: signing out disconnects the user with reconnect") {
  Fixture f;
  Cable cable(f);
  Client c(f.port());
  Reply r = c.request("POST", "/session", kSameOrigin + kForm,
                      std::string("email_address=david@example.com&password=") + kPassword);
  REQUIRE(r.status == 302);
  std::string token;
  for (const auto& [k, v] : r.headers) {
    if (k == "set-cookie" && v.starts_with("session_token=")) token = v.substr(0, v.find(';'));
  }
  REQUIRE(!token.empty());
  r = c.request("DELETE", "/session", kSameOrigin + "Cookie: " + token + "\r\n");
  CHECK(r.status == 302);
  cable.server.hub().drain(0);
  REQUIRE(cable.listener.frames.size() == 1);
  CHECK(cable.listener.frames[0] == protocol::remote_disconnect_payload(true));
}

}  // namespace campfire::app::testing
