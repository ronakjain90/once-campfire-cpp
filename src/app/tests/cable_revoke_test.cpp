// End to end tests of the three revocation paths over a real WebSocket: subscribe, revoke, try a delivery, reconnect.
// Rails: Membership after_destroy_commit, User#deactivate, User::Bannable#ban. Rust: crates/db/src/events.rs.
#include <arpa/inet.h>
#include <doctest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>

#include "app/broadcasts.hpp"
#include "app/channels/server.hpp"
#include "app/tests/fixture.hpp"
#include "cable/protocol.hpp"
#include "models/hooks.hpp"
#include "models/room.hpp"
#include "models/user_admin.hpp"

namespace campfire::app::testing {

namespace {

using channels::CableServer;

// A blocking client for the cable: no extensions, so the server sends plain frames.
class Socket {
 public:
  Socket(std::uint16_t port, const std::string& cookie) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    REQUIRE(::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    timeval tv{5, 0};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    const std::string head =
        "GET /cable HTTP/1.1\r\nHost: test.example\r\nOrigin: https://test.example\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Protocol: actioncable-v1-json\r\nCookie: " +
        cookie + "\r\n\r\n";
    raw_send(head);
    while (buffer_.find("\r\n\r\n") == std::string::npos) REQUIRE(fill());
    status_ = buffer_.substr(0, buffer_.find("\r\n"));
    buffer_.erase(0, buffer_.find("\r\n\r\n") + 4);
  }
  ~Socket() { ::close(fd_); }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  [[nodiscard]] const std::string& status() const { return status_; }

  // A masked text frame.
  void send_text(const std::string& text) {
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    if (text.size() < 126) {
      frame.push_back(static_cast<char>(0x80 | text.size()));
    } else {
      frame.push_back(static_cast<char>(0x80 | 126));
      frame.push_back(static_cast<char>(text.size() >> 8));
      frame.push_back(static_cast<char>(text.size() & 0xff));
    }
    const char mask[4] = {0x11, 0x22, 0x33, 0x44};
    frame.append(mask, 4);
    for (std::size_t i = 0; i < text.size(); ++i) frame.push_back(static_cast<char>(text[i] ^ mask[i % 4]));
    raw_send(frame);
  }

  void subscribe(const std::string& channel_json) {
    std::string escaped;
    for (const char c : channel_json) {
      if (c == '"') escaped += '\\';
      escaped += c;
    }
    send_text(R"({"command":"subscribe","identifier":")" + escaped + R"("})");
  }

  // The next text frame, or "" when the peer closed or the read timed out. Close frames end with "".
  std::string next_text() {
    while (true) {
      if (buffer_.size() >= 2) {
        const auto op = static_cast<unsigned char>(buffer_[0]) & 0x0f;
        std::size_t length = static_cast<unsigned char>(buffer_[1]) & 0x7f;
        std::size_t header = 2;
        if (length == 126 && buffer_.size() >= 4) {
          length = (static_cast<unsigned char>(buffer_[2]) << 8) | static_cast<unsigned char>(buffer_[3]);
          header = 4;
        }
        if (buffer_.size() >= header + length && !(length == 126 && header == 2)) {
          std::string payload = buffer_.substr(header, length);
          buffer_.erase(0, header + length);
          if (op == 0x8) return {};
          if (op == 0x1) return payload;
          continue;  // ping, pong
        }
      }
      if (!fill()) return {};
    }
  }

  // Reads frames until one contains `needle`. Returns it, or "" if none came.
  std::string wait_for(const std::string& needle) {
    for (std::string frame = next_text(); !frame.empty(); frame = next_text()) {
      if (frame.find(needle) != std::string::npos) return frame;
    }
    return {};
  }

  // True when the server closed the socket (a close frame or the end of the stream).
  bool ended() { return next_text().empty() && !fill(); }

 private:
  void raw_send(std::string text) {
    while (!text.empty()) {
      const ssize_t n = ::send(fd_, text.data(), text.size(), MSG_NOSIGNAL);
      REQUIRE(n > 0);
      text.erase(0, static_cast<std::size_t>(n));
    }
  }
  bool fill() {
    char chunk[4096];
    const ssize_t n = ::recv(fd_, chunk, sizeof chunk, 0);
    if (n <= 0) return false;
    buffer_.append(chunk, static_cast<std::size_t>(n));
    return true;
  }

  int fd_ = -1;
  std::string buffer_;
  std::string status_;
};

// A ban stores the address of each session, and a ban of a private address is not valid (Rails: Ban validation).
const db::Query<void()> kPublicAddress{"UPDATE sessions SET ip_address = '203.0.113.9'"};
const db::Query<void()> kInsertRoom{
    "INSERT INTO rooms (name, type, creator_id, created_at, updated_at) VALUES "
    "('Open', 'Rooms::Open', 1, '2026-03-02 16:00:00', '2026-03-02 16:00:00')"};
const db::Query<void()> kInsertMembership{
    "INSERT INTO memberships (room_id, user_id, involvement, created_at, updated_at) VALUES "
    "(1, 1, 'mentions', '2026-03-02 16:00:00', '2026-03-02 16:00:00')"};

constexpr const char* kUnread = R"({"channel":"UnreadRoomsChannel"})";
constexpr const char* kPresence = R"({"channel":"PresenceChannel","room_id":1})";

// The app with a cable that the workers drain, and a signed in user (id 1) who is a member of room 1.
struct Live {
  Live()
      : f({}, true,
          [this](net::ServerOptions& options) {
            options.on_wake = [this](unsigned worker) {
              if (CableServer* s = cable_ptr.load()) s->on_wake(worker);
            };
          }),
        cable(*f.state, 2, [this](unsigned worker) { f.server->wake(worker); }) {
    cable_ptr.store(&cable);
    models::hooks::set_disconnect_user(
        [this](std::int64_t user_id, bool reconnect) { cable.disconnect_user(user_id, reconnect); });
    f.write([](db::Tx& tx) -> Status {
      if (auto r = tx.conn().exec(kInsertRoom); !r) return std::unexpected(r.error());
      if (auto r = tx.conn().exec(kInsertMembership); !r) return std::unexpected(r.error());
      return {};
    });
    Client c(f.port());
    Reply r = c.request("POST", "/session", kSameOrigin + kForm,
                        std::string("email_address=david@example.com&password=") + kPassword);
    REQUIRE(r.status == 302);
    for (const auto& [k, v] : r.headers) {
      if (k == "set-cookie" && v.starts_with("session_token=")) cookie = v.substr(0, v.find(';'));
    }
    REQUIRE(!cookie.empty());
  }
  ~Live() {
    // The workers use the hub: stop them first.
    f.server->stop();
    models::hooks::set_disconnect_user(nullptr);
    cable_ptr.store(nullptr);
  }
  // Opens a socket and waits for the welcome frame.
  std::unique_ptr<Socket> connect() {
    auto socket = std::make_unique<Socket>(f.server->target_port(), cookie);
    REQUIRE(socket->status().starts_with("HTTP/1.1 101"));
    return socket;
  }
  std::atomic<CableServer*> cable_ptr{nullptr};
  Fixture f;
  CableServer cable;
  std::string cookie;
};

}  // namespace

TEST_CASE("cable e2e: removing a membership disconnects the socket and the room stays closed to a new one") {
  Live live;
  auto socket = live.connect();
  REQUIRE(!socket->wait_for("welcome").empty());
  socket->subscribe(kPresence);
  REQUIRE(!socket->wait_for("confirm_subscription").empty());

  models::Room room;
  room.id = 1;
  const std::int64_t revoked[] = {1};
  live.f.write([&](db::Tx& tx) -> Status { return models::rooms::revise(tx, room, {}, revoked); });
  CHECK(!socket->wait_for(R"("reason":"remote","reconnect":true)").empty());
  CHECK(socket->ended());

  // A delivery to a stream of the user: the old socket is closed and gets nothing, a new socket gets it.

  // The user is still signed in: the socket opens, but the room rejects the subscription.
  auto again = live.connect();
  REQUIRE(!again->wait_for("welcome").empty());
  again->subscribe(kPresence);
  CHECK(!again->wait_for("reject_subscription").empty());
  again->subscribe(kUnread);
  REQUIRE(!again->wait_for("confirm_subscription").empty());
  live.cable.hub().broadcast_encoded(broadcasts::unread_rooms_stream(1), R"({"probe":1})");
  CHECK(!again->wait_for(R"("message":{"probe":1})").empty());
}

TEST_CASE("cable e2e: deactivating a user disconnects the socket and refuses a new one") {
  Live live;
  auto socket = live.connect();
  REQUIRE(!socket->wait_for("welcome").empty());
  socket->subscribe(kPresence);
  REQUIRE(!socket->wait_for("confirm_subscription").empty());

  live.f.write([](db::Tx& tx) -> Status { return models::users::deactivate(tx, 1); });
  CHECK(!socket->wait_for(R"("reason":"remote","reconnect":false)").empty());
  CHECK(socket->ended());

  live.cable.hub().broadcast_encoded(broadcasts::unread_rooms_stream(1), R"({"probe":1})");

  // The sessions are gone: the new socket gets the unauthorized disconnect.
  auto again = live.connect();
  CHECK(!again->wait_for(R"("reason":"unauthorized")").empty());
  CHECK(again->ended());
}

TEST_CASE("cable e2e: banning a user disconnects the socket and refuses a new one") {
  Live live;
  auto socket = live.connect();
  REQUIRE(!socket->wait_for("welcome").empty());
  socket->subscribe(kPresence);
  REQUIRE(!socket->wait_for("confirm_subscription").empty());

  live.f.write([](db::Tx& tx) -> Status {
    if (auto r = tx.conn().exec(kPublicAddress); !r) return std::unexpected(r.error());
    return models::users::ban(tx, 1);
  });
  CHECK(!socket->wait_for(R"("reason":"remote","reconnect":false)").empty());
  CHECK(socket->ended());

  live.cable.hub().broadcast_encoded(broadcasts::unread_rooms_stream(1), R"({"probe":1})");

  auto again = live.connect();
  CHECK(!again->wait_for(R"("reason":"unauthorized")").empty());
  CHECK(again->ended());
}

}  // namespace campfire::app::testing
