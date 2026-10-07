// Tests of the push and webhook delivery against local servers: the TLS push service decrypts what it gets, and the
// webhook server records the request. Rails: lib/web_push, app/models/webhook.rb. Rust:
// crates/campfire/src/integrations/{web_push/tests.rs,webhook.rs}.
#include <doctest.h>

#include <chrono>
#include <fstream>
#include <set>
#include <sstream>

#include "app/job_runner.hpp"
#include "app/tests/fake_http_server.hpp"
#include "app/tests/fixture.hpp"
#include "app/web_push.hpp"
#include "app/webhook.hpp"
#include "compat/base64.hpp"
#include "compat/json.hpp"
#include "jobs/web_push.hpp"
#include "storage/filename.hpp"

namespace campfire::app::testing {

namespace {

namespace b64 = compat::base64;
using compat::json::Value;

constexpr std::string_view kVapidPublic =
    "BEYXTBB5_jNhNzXDmx5KEU55Vbbd-u--Lk9rM5OFQvUkPIBwZJ9QzAq0zdEzFw6yTV8cTriz_qYBVicY02_VxTQ=";
constexpr std::string_view kVapidPrivate = "qfXLHghuG1rSHZUVo9SscNRI-0EIHRbIrfeGCqbAwak=";
constexpr const char* kPublicIp = "142.250.185.206";

const std::string kTlsDir = std::string(CAMPFIRE_SPEC_DIR) + "/vectors/tls/";

Value read_json(const std::string& name) {
  std::ifstream file(std::string(CAMPFIRE_SPEC_DIR) + "/vectors/" + name);
  REQUIRE(file.good());
  std::stringstream text;
  text << file.rdbuf();
  auto parsed = compat::json::parse(text.str());
  REQUIRE(parsed);
  return std::move(*parsed);
}

std::string address_bytes(const std::string& text) {
  in_addr a{};
  REQUIRE(inet_pton(AF_INET, text.c_str(), &a) == 1);
  return std::string(reinterpret_cast<const char*>(&a), 4);
}

// The user agent of a subscription: the keys that the push service decrypts with.
struct Receiver {
  jobs::web_push::KeyPair key = jobs::web_push::generate_key_pair();
  std::string auth = "0123456789abcdef";

  [[nodiscard]] models::PushSubscription subscription(std::int64_t id, std::string endpoint) const {
    models::PushSubscription s;
    s.id = id;
    s.user_id = 2;
    s.endpoint = std::move(endpoint);
    s.p256dh_key = b64::urlsafe_encode_unpadded(key.public_key);
    s.auth_key = b64::urlsafe_encode_unpadded(auth);
    return s;
  }
  // The message that the body holds: the plaintext without the padding of the gem.
  [[nodiscard]] std::string open(const std::string& body) const {
    auto opened = jobs::web_push::decrypt(body, key.private_key, auth);
    REQUIRE(opened.has_value());
    REQUIRE(opened->plaintext.ends_with(std::string("\x02\x00", 2)));
    return opened->plaintext.substr(0, opened->plaintext.size() - 2);
  }
};

// A push service on a local port that speaks TLS with a certificate for fcm.googleapis.com.
struct PushService {
  explicit PushService(int status, const std::string& reason = "Created")
      : server({{"POST", "*", "/fcm/send/abc", status, {}, "", reason}}, kTlsDir + "server.pem",
               kTlsDir + "server.key") {
    network.lookup = [this](const std::string& host) {
      const std::scoped_lock lock(mutex);
      lookups.push_back(host);
      if (host == "fcm.googleapis.com") return std::vector<std::string>{address_bytes(kPublicIp)};
      if (host == "updates.push.services.mozilla.com") return std::vector<std::string>{address_bytes("10.0.0.5")};
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
  std::mutex mutex;
  std::vector<std::string> lookups;
};

jobs::web_push::Vapid make_vapid() {
  auto vapid = jobs::web_push::Vapid::create("mailto:support@37signals.com", kVapidPublic, kVapidPrivate);
  REQUIRE(vapid.has_value());
  return std::move(*vapid);
}

web_push::Notification notification_for(const models::PushSubscription& subscription) {
  web_push::Notification n;
  n.title = "Designers <&> \"quotes\" \xC3\xA9 \xF0\x9F\x98\x80";
  n.body = "Kevin: line\nbreak\ttab \xE2\x80\xA8 \x1f / \\ ";
  n.path = "/rooms/1";
  n.badge = 3;
  n.subscription = subscription;
  return n;
}

}  // namespace

TEST_CASE("web push: delivers to the pinned address with the headers of the gem") {
  PushService service(201);
  const Receiver receiver;
  const auto vapid = make_vapid();
  const auto notification = notification_for(receiver.subscription(1, "https://fcm.googleapis.com/fcm/send/abc"));
  const auto delivered = web_push::deliver(notification, vapid, service.network, 1700000000);
  REQUIRE(delivered.has_value());
  CHECK(*delivered == std::optional<int>(201));
  CHECK(service.lookups == std::vector<std::string>{"fcm.googleapis.com"});

  const auto received = service.server.received();
  REQUIRE(received.size() == 1);
  const auto& request = received[0];
  CHECK(request.method == "POST");
  CHECK(request.target == "/fcm/send/abc");
  std::vector<std::string> names;
  for (const auto& header : request.headers) names.push_back(header.first);
  CHECK(names == std::vector<std::string>{"Content-Type", "Ttl", "Urgency", "Content-Encoding", "Content-Length",
                                          "Authorization", "Accept-Encoding", "Accept", "User-Agent", "Connection",
                                          "Host"});
  CHECK(request.header("Content-Type") == "application/octet-stream");
  CHECK(request.header("Ttl") == "2419200");
  CHECK(request.header("Urgency") == "high");
  CHECK(request.header("Content-Encoding") == "aes128gcm");
  CHECK(request.header("Content-Length") == std::to_string(request.body.size()));
  CHECK(request.header("Host") == "fcm.googleapis.com");
  CHECK(request.header("User-Agent") == "Ruby");
  CHECK(request.header("Authorization").starts_with("vapid t=eyJ0eXAiOiJKV1QiLCJhbGciOiJFUzI1NiJ9.eyJhdWQiOiJ"));
  // The bytes of the Rust port and of the gem for this notification.
  CHECK(receiver.open(request.body) ==
        "{\"title\":\"Designers <&> \\\"quotes\\\" \xC3\xA9 \xF0\x9F\x98\x80\",\"options\":{\"body\":\"Kevin: "
        "line\\nbreak\\ttab \xE2\x80\xA8 \\u001f / \\\\ \",\"icon\":\"/account/logo\",\"data\":{\"path\":\"/rooms/"
        "1\",\"badge\":3}}}");
}

TEST_CASE("web push: skips the endpoints that it may not deliver to") {
  PushService service(201);
  const Receiver receiver;
  const auto vapid = make_vapid();
  for (const char* endpoint :
       {"https://updates.push.services.mozilla.com/wpush/v2/x",  // resolves to a private address
        "https://web.push.apple.com/QaBC123",                    // does not resolve
        "https://attacker.example.com/collect", "https://fcm.googleapis.com:22/fcm/send/abc",
        "http://fcm.googleapis.com/fcm/send/abc", "https://evilfcm.googleapis.com.attacker.example/webhook"}) {
    const auto delivered =
        web_push::deliver(notification_for(receiver.subscription(1, endpoint)), vapid, service.network, 0);
    REQUIRE(delivered.has_value());
    CHECK_MESSAGE(!delivered->has_value(), endpoint);
  }
  CHECK(service.server.received().empty());
  CHECK(service.lookups == std::vector<std::string>{"updates.push.services.mozilla.com", "web.push.apple.com"});
}

TEST_CASE("web push: raises what the gem raises") {
  struct Case {
    int status;
    const char* reason;
    const char* name;
    bool invalidates;
  };
  const Receiver receiver;
  const auto vapid = make_vapid();
  for (const Case& c : {Case{410, "Gone", "WebPush::ExpiredSubscription", true},
                        Case{404, "Not Found", "WebPush::InvalidSubscription", true},
                        Case{403, "Forbidden", "WebPush::Unauthorized", false},
                        Case{400, "UnauthorizedRegistration", "WebPush::Unauthorized", false},
                        Case{400, "Bad Request", "WebPush::ResponseError", false},
                        Case{413, "Payload Too Large", "WebPush::PayloadTooLarge", false},
                        Case{429, "Too Many Requests", "WebPush::TooManyRequests", false},
                        Case{503, "Service Unavailable", "WebPush::PushServiceError", false},
                        Case{302, "Found", "WebPush::ResponseError", false}}) {
    PushService service(c.status, c.reason);
    const auto delivered =
        web_push::deliver(notification_for(receiver.subscription(1, "https://fcm.googleapis.com/fcm/send/abc")), vapid,
                          service.network, 0);
    REQUIRE_MESSAGE(!delivered.has_value(), c.status);
    CHECK(delivered.error().class_name == c.name);
    CHECK(delivered.error().invalidates_subscription() == c.invalidates);
  }
}

TEST_CASE("web push: a bad key can never be delivered to, and a bad certificate may be our fault") {
  PushService service(201);
  const auto vapid = make_vapid();
  const Receiver receiver;
  models::PushSubscription bad_key = receiver.subscription(1, "https://fcm.googleapis.com/fcm/send/abc");
  bad_key.p256dh_key = "dGVzdF9rZXk";
  auto delivered = web_push::deliver(notification_for(bad_key), vapid, service.network, 0);
  REQUIRE(!delivered.has_value());
  CHECK(delivered.error().class_name == "OpenSSL::PKey::EC::Point::Error");
  CHECK(delivered.error().invalidates_subscription());

  unfurl::Network untrusted = service.network;
  untrusted.ca_file = kTlsDir + "server.pem";  // not the CA of the server certificate
  delivered = web_push::deliver(notification_for(receiver.subscription(1, "https://fcm.googleapis.com/fcm/send/abc")),
                                vapid, untrusted, 0);
  REQUIRE(!delivered.has_value());
  CHECK(delivered.error().class_name == "OpenSSL::SSL::SSLError");
  CHECK_FALSE(delivered.error().invalidates_subscription());

  models::PushSubscription blank = receiver.subscription(1, "https://fcm.googleapis.com/fcm/send/abc");
  blank.auth_key = "";
  delivered = web_push::deliver(notification_for(blank), vapid, service.network, 0);
  REQUIRE(!delivered.has_value());
  CHECK(delivered.error().class_name == "ArgumentError");
  CHECK_FALSE(delivered.error().invalidates_subscription());
}

namespace {

std::string decode(const std::string& text) {
  return *b64::strict_decode(text);
}

// The routes of the webhook cases: each case answers on "/<name>".
std::vector<test::FakeRoute> webhook_routes(const Value& cases) {
  std::vector<test::FakeRoute> routes;
  for (const Value& c : cases.as_array()) {
    test::FakeRoute route;
    route.method = "POST";
    route.host = "*";
    route.path = "/" + c.find("name")->as_string();
    route.status = static_cast<int>(*c.find("status")->to_int64());
    for (const Value& header : c.find("headers")->as_array()) {
      route.headers.emplace_back(header.as_array()[0].as_string(), header.as_array()[1].as_string());
    }
    if (const Value* body = c.find("body_b64")) {
      route.body = decode(body->as_string());
    } else if (const Value* text = c.find("body")) {
      route.body = text->as_string();
    }
    if (const Value* gzip = c.find("gzip")) route.gzip = gzip->as_bool();
    if (const Value* delay = c.find("delay")) route.delay = std::chrono::seconds(*delay->to_int64());
    routes.push_back(std::move(route));
  }
  return routes;
}

}  // namespace

TEST_CASE("webhook: the replies of the Rails reference") {
  const Value cases = read_json("webhook_cases.json");
  const Value expected = read_json("webhook_expected.json");
  test::FakeServer server(webhook_routes(cases));
  const unfurl::Network network;
  const std::string payload = "{\"message\":\"hi\"}";
  REQUIRE(cases.as_array().size() == expected.as_array().size());
  for (std::size_t i = 0; i < cases.as_array().size(); ++i) {
    const Value& c = cases.as_array()[i];
    const Value& want = expected.as_array()[i];
    const std::string name = c.find("name")->as_string();
    INFO(name);
    const Value* url_value = c.find("url");
    const std::string url = url_value != nullptr ? url_value->as_string()
                                                 : "http://127.0.0.1:" + std::to_string(server.port()) + "/" + name;
    const auto delivery = webhook::deliver(network, url, payload);
    if (const Value* error = want.find("error")) {
      REQUIRE_FALSE(delivery.has_value());
      if (error->as_string() == "Mime::Type::InvalidMimeType") {
        CHECK(delivery.error().message.find("is not a valid MIME type") != std::string::npos);
      } else {
        CHECK(error->as_string() == "Errno::ECONNREFUSED");
        CHECK(delivery.error().code == Errc::Io);
      }
      continue;
    }
    REQUIRE(delivery.has_value());
    const Value* status = want.find("status");
    if (status->is_null()) {
      CHECK_FALSE(delivery->status.has_value());
    } else {
      CHECK(delivery->status == std::optional<int>(static_cast<int>(*status->to_int64())));
    }
    const Value* reply = want.find("reply");
    using Kind = jobs::webhook::Reply::Kind;
    if (reply->is_null()) {
      CHECK(delivery->reply.kind == Kind::None);
    } else if (const Value* attachment = reply->find("attachment")) {
      REQUIRE(delivery->reply.kind == Kind::Attachment);
      CHECK(delivery->reply.attachment.filename == attachment->find("filename")->as_string());
      CHECK(delivery->reply.attachment.content_type == attachment->find("content_type")->as_string());
      CHECK(delivery->reply.attachment.data == decode(attachment->find("body_b64")->as_string()));
    } else {
      REQUIRE(delivery->reply.kind == Kind::Text);
      CHECK(delivery->reply.text == storage::utf8_lossy(decode(reply->find("text_b64")->as_string())));
    }
  }

  // The request as Net::HTTP sends it.
  const auto& first = expected.as_array()[0].find("requests")->as_array()[0];
  const auto received = server.received();
  const test::Received* request = nullptr;
  for (const auto& r : received) {
    if (r.target == "/text") request = &r;
  }
  REQUIRE(request != nullptr);
  const auto& wanted = first.find("headers")->as_array();
  REQUIRE(request->headers.size() == wanted.size());
  for (std::size_t i = 0; i < wanted.size(); ++i) {
    CHECK(request->headers[i].first == wanted[i].as_array()[0].as_string());
    const std::string value = request->headers[i].first == "Host" ? "127.0.0.1:" + std::to_string(server.port())
                                                                  : wanted[i].as_array()[1].as_string();
    CHECK(request->headers[i].second == value);
  }
  CHECK(request->body == first.find("body")->as_string());
  CHECK(request->head.starts_with("POST /text HTTP/1.1\r\nContent-Type: application/json\r\nAccept-Encoding: "));
}

TEST_CASE("webhook: the whole delivery has a deadline, and a bad URL fails") {
  test::FakeRoute trickle;
  trickle.method = "POST";
  trickle.host = "*";
  trickle.path = "/hook";
  trickle.headers = {{"Content-Type", "image/png"}};
  trickle.trickle = true;
  test::FakeServer server({trickle});
  const unfurl::Network network;
  const auto url = "http://127.0.0.1:" + std::to_string(server.port()) + "/hook";
  const auto delivery = webhook::deliver(network, url, "{}", std::chrono::seconds(1));
  REQUIRE(delivery.has_value());
  CHECK_FALSE(delivery->status.has_value());
  CHECK(delivery->reply.text == "Failed to respond within 1 seconds");

  CHECK_FALSE(webhook::deliver(network, "ftp://example.com/x", "{}").has_value());
  CHECK_FALSE(webhook::deliver(network, "not a url", "{}").has_value());
}

}  // namespace campfire::app::testing
