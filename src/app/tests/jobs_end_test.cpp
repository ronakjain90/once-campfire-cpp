// End to end tests of the jobs: the app makes a message, and a job queue pushes it, posts it to a bot, purges a blob or
// removes the content of a banned user. The servers are local. Rails: app/jobs/**, app/models/room.rb,
// app/models/webhook.rb, app/models/user/bannable.rb.
#include <doctest.h>

#include <filesystem>

#include "app/job_runner.hpp"
#include "app/tests/fake_http_server.hpp"
#include "app/tests/fixture.hpp"
#include "compat/base64.hpp"
#include "jobs/web_push.hpp"
#include "models/user_admin.hpp"

namespace campfire::app::testing {

namespace {

namespace b64 = compat::base64;
using std::chrono_literals::operator""s;

constexpr std::string_view kVapidPublic =
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8cTriz_qYBVicY02_VxTQ=";
constexpr std::string_view kVapidPrivate = "qfXLHghuG1rSHZUVo9SscNRI-0EIHRbIrfeGCqbAwak=";
constexpr const char* kPublicIp = "142.250.185.206";
const std::string kTlsDir = std::string(CAMPFIRE_TESTS_DIR) + "/vectors/tls/";

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

// David (administrator, id 1) is in the account. Jason (member, id 2) and Bender (bot, id 3) join room 1, "Designers".
void seed_room(Fixture& f, const std::string& involvement = "everything") {
  const std::string digest = req::bcrypt::hash_password(kPassword, req::bcrypt::kMinCost);
  f.write([&](db::Tx& tx) -> Status {
    if (auto r = tx.conn().exec(kInsertUser, "Jason", "jason@example.com", digest, 0); !r)
      return std::unexpected(r.error());
    return tx.conn().exec_sql(
        "INSERT INTO users (name, role, status, bot_token, created_at, updated_at) VALUES "
        "('Bender', 2, 0, 'BenderToken', '2026-03-02 16:00:00', '2026-03-02 16:00:00');"
        "INSERT INTO rooms (name, type, creator_id, created_at, updated_at) VALUES "
        "('Designers', 'Rooms::Open', 1, '2026-03-02 16:00:00', '2026-03-02 16:00:00');"
        "INSERT INTO memberships (room_id, user_id, involvement, connections, created_at, updated_at) VALUES "
        "(1, 1, 'everything', 0, '2026-03-02 16:00:00', '2026-03-02 16:00:00'), "
        "(1, 2, '" +
        involvement +
        "', 0, '2026-03-02 16:00:00', '2026-03-02 16:00:00'), "
        "(1, 3, 'everything', 0, '2026-03-02 16:00:00', '2026-03-02 16:00:00');");
  });
}

std::string scalar(Fixture& f, const std::string& sql) {
  auto conn = f.state->db->open_reader();
  REQUIRE(conn.has_value());
  sqlite3_stmt* st = nullptr;
  REQUIRE(sqlite3_prepare_v2(conn->handle(), sql.c_str(), -1, &st, nullptr) == SQLITE_OK);
  std::string out = "<none>";
  if (sqlite3_step(st) == SQLITE_ROW) {
    const auto* text = sqlite3_column_text(st, 0);
    out = text != nullptr ? reinterpret_cast<const char*>(text) : "<null>";
  }
  sqlite3_finalize(st);
  return out;
}

JobRunner& runner(Fixture& f) {
  auto* r = dynamic_cast<JobRunner*>(f.state->job_sink.get());
  REQUIRE(r != nullptr);
  return *r;
}

std::string address_bytes(const std::string& text) {
  in_addr a{};
  REQUIRE(inet_pton(AF_INET, text.c_str(), &a) == 1);
  return std::string(reinterpret_cast<const char*>(&a), 4);
}

struct PushService {
  explicit PushService(int status, const std::string& reason = "Created")
      : server({{"POST", "*", "/fcm/send/abc", status, {}, "", reason}}, kTlsDir + "server.pem",
               kTlsDir + "server.key") {
    network.lookup = [](const std::string& host) {
      if (host == "fcm.googleapis.com") return std::vector<std::string>{address_bytes(kPublicIp)};
      return std::vector<std::string>{};
    };
    const std::uint16_t port = server.port();
    network.dial_override = [port](std::string& ip, std::uint16_t& target) {
      if (ip == kPublicIp) {
        ip = "127.0.0.1";
        target = port;
      }
    };
    network.ca_file = kTlsDir + "ca.pem";
  }
  test::FakeServer server;
  unfurl::Network network;
};

struct Receiver {
  jobs::web_push::KeyPair key = jobs::web_push::generate_key_pair();
  std::string auth = "0123456789abcdef";
  [[nodiscard]] std::string open(const std::string& body) const {
    auto opened = jobs::web_push::decrypt(body, key.private_key, auth);
    REQUIRE(opened.has_value());
    return opened->plaintext.substr(0, opened->plaintext.size() - 2);
  }
};

// Jason's subscription for the keys of `receiver`.
void subscribe_jason(Fixture& f, const Receiver& receiver) {
  f.write([&](db::Tx& tx) -> Status {
    return tx.conn().exec_sql(
        "INSERT INTO push_subscriptions (endpoint, p256dh_key, auth_key, user_id, created_at, updated_at) VALUES "
        "('https://fcm.googleapis.com/fcm/send/abc', '" +
        b64::urlsafe_encode_unpadded(receiver.key.public_key) + "', '" + b64::urlsafe_encode_unpadded(receiver.auth) +
        "', 2, '2026-03-02 16:00:00', '2026-03-02 16:00:00');");
  });
}

void enable_push(Fixture& f, PushService& service) {
  auto vapid = jobs::web_push::Vapid::create("mailto:support@37signals.com", kVapidPublic, kVapidPrivate);
  REQUIRE(vapid.has_value());
  f.state->vapid = std::make_shared<const jobs::web_push::Vapid>(std::move(*vapid));
  f.state->push_network = service.network;
}

Reply post_message(Client& c, const std::string& cookie, const std::string& body) {
  return c.request("POST", "/rooms/1/messages", cookie + kSameOrigin + kForm + "Accept: text/vnd.turbo-stream.html\r\n",
                   "message%5Bbody%5D=" + body);
}

}  // namespace

TEST_CASE("jobs: a message is pushed to the subscriptions of the members who want everything") {
  Fixture f;
  seed_room(f);
  PushService service(201);
  const Receiver receiver;
  subscribe_jason(f, receiver);
  enable_push(f, service);
  Client c(f.port());
  const std::string david = sign_in(c, "david@example.com");

  REQUIRE(post_message(c, david, "%3Cp%3EHello%3C%2Fp%3E").status == 200);
  REQUIRE(runner(f).wait_idle(20s));
  const auto received = service.server.received();
  REQUIRE(received.size() == 1);
  // The title and the body of a shared room, the path of the room, and the unread count of Jason as the badge.
  CHECK(receiver.open(received[0].body) ==
        "{\"title\":\"Designers\",\"options\":{\"body\":\"David: Hello\",\"icon\":\"/account/logo\",\"data\":{\"path\":"
        "\"/rooms/1\",\"badge\":1}}}");
}

TEST_CASE("jobs: nothing is pushed to a connected member or to a member who wants mentions only") {
  Fixture f;
  seed_room(f, "mentions");
  PushService service(201);
  const Receiver receiver;
  subscribe_jason(f, receiver);
  enable_push(f, service);
  Client c(f.port());
  const std::string david = sign_in(c, "david@example.com");

  REQUIRE(post_message(c, david, "%3Cp%3EHello%3C%2Fp%3E").status == 200);
  REQUIRE(runner(f).wait_idle(20s));
  CHECK(service.server.received().empty());

  // Jason wants everything, but he has a socket open: `connected_at` is less than a minute old.
  f.write([](db::Tx& tx) -> Status {
    return tx.conn().exec_sql(
        "UPDATE memberships SET involvement = 'everything', connected_at = '2026-03-02 15:59:30' WHERE user_id = 2");
  });
  REQUIRE(post_message(c, david, "%3Cp%3EAgain%3C%2Fp%3E").status == 200);
  REQUIRE(runner(f).wait_idle(20s));
  CHECK(service.server.received().empty());
}

TEST_CASE("jobs: a push service that does not know the subscription destroys it") {
  Fixture f;
  seed_room(f);
  PushService service(410, "Gone");
  const Receiver receiver;
  subscribe_jason(f, receiver);
  enable_push(f, service);
  Client c(f.port());
  const std::string david = sign_in(c, "david@example.com");
  REQUIRE(scalar(f, "SELECT COUNT(*) FROM push_subscriptions") == "1");

  REQUIRE(post_message(c, david, "%3Cp%3EHello%3C%2Fp%3E").status == 200);
  REQUIRE(runner(f).wait_idle(20s));
  CHECK(service.server.received().size() == 1);
  CHECK(scalar(f, "SELECT COUNT(*) FROM push_subscriptions") == "0");
}

TEST_CASE("jobs: Web Push is off without VAPID keys") {
  Fixture f;
  seed_room(f);
  PushService service(201);
  const Receiver receiver;
  subscribe_jason(f, receiver);
  f.state->push_network = service.network;  // no keys: `app.vapid` stays empty
  Client c(f.port());
  const std::string david = sign_in(c, "david@example.com");
  REQUIRE(post_message(c, david, "%3Cp%3EHello%3C%2Fp%3E").status == 200);
  REQUIRE(runner(f).wait_idle(20s));
  CHECK(service.server.received().empty());
}

TEST_CASE("jobs: the test notification is delivered in the request") {
  Fixture f;
  seed_room(f);
  PushService service(201);
  const Receiver receiver;
  subscribe_jason(f, receiver);
  enable_push(f, service);
  Client c(f.port());
  const std::string jason = sign_in(c, "jason@example.com");
  const Reply r =
      c.request("POST", "/users/me/push_subscriptions/1/test_notifications", jason + kSameOrigin + kForm, "x=1");
  REQUIRE(r.status == 302);
  CHECK(r.header("location") == "http://test.example/users/me/push_subscriptions");
  const auto received = service.server.received();
  REQUIRE(received.size() == 1);
  const std::string message = receiver.open(received[0].body);
  CHECK(message.starts_with("{\"title\":\"Campfire Test\",\"options\":{\"body\":\""));
  CHECK(message.ends_with(
      "\",\"icon\":\"/account/logo\",\"data\":{\"path\":\"http://test.example/users/me/push_subscriptions\","
      "\"badge\":0}}}"));
}

TEST_CASE("jobs: a bot gets the message and its reply becomes a message") {
  Fixture f;
  seed_room(f);
  test::FakeRoute hook;
  hook.method = "POST";
  hook.host = "*";
  hook.path = "/hook";
  hook.headers = {{"Content-Type", "text/plain"}};
  hook.body = "Hello back!";
  test::FakeServer server({hook});
  f.write([&](db::Tx& tx) -> Status {
    return tx.conn().exec_sql("INSERT INTO webhooks (url, user_id, created_at, updated_at) VALUES ('http://127.0.0.1:" +
                              std::to_string(server.port()) +
                              "/hook', 3, '2026-03-02 16:00:00', '2026-03-02 16:00:00');");
  });
  Client c(f.port());
  const std::string david = sign_in(c, "david@example.com");
  REQUIRE(post_message(c, david, "%3Cp%3EHello%3C%2Fp%3E").status == 200);
  runner(f).deliver_webhook(3, 1);
  REQUIRE(runner(f).wait_idle(20s));

  const auto received = server.received();
  REQUIRE(received.size() == 1);
  CHECK(received[0].header("Content-Type") == "application/json");
  CHECK(received[0].body ==
        "{\"user\":{\"id\":1,\"name\":\"David\"},\"room\":{\"id\":1,\"name\":\"Designers\",\"path\":"
        "\"/rooms/1/3-BenderToken/messages\"},\"message\":{\"id\":1,\"body\":{\"html\":\"\\u003cp\\u003eHello\\u003c/"
        "p\\u003e\",\"plain\":\"Hello\"},\"path\":\"/rooms/1/@1\"}}");
  CHECK(scalar(f, "SELECT creator_id FROM messages WHERE id = 2") == "3");
  CHECK(scalar(f, "SELECT body FROM action_text_rich_texts WHERE record_id = 2 AND record_type = 'Message'") ==
        "Hello back!");
  CHECK(scalar(f, "SELECT body FROM message_search_index WHERE rowid = 2") == "Hello back!");
}

TEST_CASE("jobs: an attachment reply is stored, processed, and purged with its message") {
  Fixture f;
  seed_room(f);
  // A 1 by 1 PNG.
  const std::string png = *b64::strict_decode(
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==");
  test::FakeRoute hook;
  hook.method = "POST";
  hook.host = "*";
  hook.path = "/hook";
  hook.headers = {{"Content-Type", "image/png"}};
  hook.body = png;
  test::FakeServer server({hook});
  f.write([&](db::Tx& tx) -> Status {
    return tx.conn().exec_sql("INSERT INTO webhooks (url, user_id, created_at, updated_at) VALUES ('http://127.0.0.1:" +
                              std::to_string(server.port()) +
                              "/hook', 3, '2026-03-02 16:00:00', '2026-03-02 16:00:00');");
  });
  Client c(f.port());
  const std::string david = sign_in(c, "david@example.com");
  REQUIRE(post_message(c, david, "%3Cp%3EHello%3C%2Fp%3E").status == 200);
  runner(f).deliver_webhook(3, 1);
  REQUIRE(runner(f).wait_idle(20s));

  CHECK(scalar(f, "SELECT creator_id FROM messages WHERE id = 2") == "3");
  CHECK(scalar(f, "SELECT filename FROM active_storage_blobs WHERE id = 1") == "attachment.png");
  CHECK(scalar(f, "SELECT content_type FROM active_storage_blobs WHERE id = 1") == "image/png");
  CHECK(scalar(f, "SELECT COUNT(*) FROM active_storage_attachments WHERE record_type = 'Message' AND record_id = 2") ==
        "1");
  CHECK(scalar(f, "SELECT metadata FROM active_storage_blobs WHERE id = 1").find("\"analyzed\":true") !=
        std::string::npos);

  // `dependent: :purge_later`: the destroy queues the purge job, which deletes the blob rows and the files.
  const Reply r =
      c.request("DELETE", "/rooms/1/messages/2", david + kSameOrigin + "Accept: text/vnd.turbo-stream.html\r\n");
  REQUIRE(r.status == 200);
  REQUIRE(runner(f).wait_idle(20s));
  CHECK(scalar(f, "SELECT COUNT(*) FROM active_storage_blobs") == "0");
  std::size_t files = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(f.state->config.storage.files)) {
    if (entry.is_regular_file()) ++files;
  }
  CHECK(files == 0);
}

TEST_CASE("jobs: a ban removes the messages of the user") {
  Fixture f;
  seed_room(f);
  Client c(f.port());
  const std::string jason = sign_in(c, "jason@example.com");
  REQUIRE(post_message(c, jason, "%3Cp%3EOne%3C%2Fp%3E").status == 200);
  REQUIRE(post_message(c, jason, "%3Cp%3ETwo%3C%2Fp%3E").status == 200);
  REQUIRE(runner(f).wait_idle(20s));
  REQUIRE(scalar(f, "SELECT COUNT(*) FROM messages WHERE creator_id = 2") == "2");

  // A ban keeps the address of each session, which must be public.
  f.write([](db::Tx& tx) { return tx.conn().exec_sql("UPDATE sessions SET ip_address = '8.8.8.8'"); });
  f.write([](db::Tx& tx) { return models::users::ban(tx, 2); });
  REQUIRE(runner(f).wait_idle(20s));
  CHECK(scalar(f, "SELECT COUNT(*) FROM messages WHERE creator_id = 2") == "0");
  CHECK(scalar(f, "SELECT COUNT(*) FROM message_search_index") == "0");
}

}  // namespace campfire::app::testing
