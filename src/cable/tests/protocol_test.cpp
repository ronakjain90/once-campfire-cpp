// Tests of the Action Cable protocol through Connection. Rust: crates/cable/tests/protocol.rs and
// src/naming.rs tests. The Rust tests that need HTTP (404 pages, origin check) belong to T5.
#include <doctest.h>

#include "cable/connection.hpp"
#include "cable/protocol.hpp"
#include "cable/tests/support.hpp"

using namespace campfire;
using namespace campfire::cable;
using namespace campfire::cable::testing;
namespace json = campfire::compat::json;

namespace {

struct Log {
  std::vector<std::string> lines;
};

struct Fixture {
  struct Room : Channel {
    explicit Room(Log& l) : log(l) {}
    Status subscribed(Subscription& sub) override {
      const json::Value* id = sub.param("room_id");
      auto n = id != nullptr ? id->to_int64() : std::nullopt;
      if (n && *n == 1) {
        std::string_view parts[] = {"room-1"};
        sub.stream_for(parts);
      } else {
        sub.reject();
      }
      log.lines.push_back(std::string("subscribed rejected=") + (sub.rejected() ? "true" : "false"));
      return {};
    }
    Status unsubscribed(Subscription& sub) override {
      log.lines.push_back(std::string("unsubscribed rejected=") + (sub.rejected() ? "true" : "false"));
      return {};
    }
    Result<bool> perform(std::string_view action, const json::Value& data, Subscription& sub) override {
      if (action == "echo") {
        sub.transmit(data);
      } else if (action == "receive") {
        sub.transmit(json::Value(json::Value::Object{{"received", data}}));
      } else if (action == "boom") {
        return fail(Errc::Internal, "boom");
      } else {
        return false;
      }
      return true;
    }
    Log& log;
  };

  Fixture() : hub(2, nullptr) {
    registry.add("RoomChannel", [this] { return std::make_unique<Room>(log); });
    registry.add("HeartbeatChannel", [] { return std::make_unique<EmptyChannel>(); });
  }
  std::unique_ptr<Connection> connect(unsigned worker, MockTransport& t, bool deflate = false,
                                      std::string_view id = "user-1") {
    auto c = std::make_unique<Connection>(hub, worker, t, registry, deflate);
    c->open(std::make_shared<const int>(1), id);
    return c;
  }
  Hub hub;
  ChannelRegistry registry;
  Log log;
};

std::string cmd(std::string_view command, std::string_view id, std::string_view data = {}) {
  json::Value::Object o{{"command", command}, {"identifier", id}};
  if (!data.empty()) o.emplace_back("data", data);
  return text_frame(json::generate(json::Value(std::move(o))));
}

std::string confirm(std::string_view id) {
  return "T:" + std::string(R"({"identifier":)") + json::encode(json::Value(id)) + R"(,"type":"confirm_subscription"})";
}
std::string reject(std::string_view id) {
  return "T:" + std::string(R"({"identifier":)") + json::encode(json::Value(id)) + R"(,"type":"reject_subscription"})";
}
std::string message(std::string_view id, std::string_view m) {
  return "T:" + std::string(R"({"identifier":)") + json::encode(json::Value(id)) + R"(,"message":)" + std::string(m) + "}";
}

constexpr std::string_view kRoom1 = R"({"channel":"RoomChannel","room_id":1})";
constexpr std::string_view kRoom2 = R"({"channel":"RoomChannel","room_id":2})";
using Frames = std::vector<std::string>;

}  // namespace

TEST_CASE("welcome, subscribe, and duplicates are ignored") {
  Fixture f;
  MockTransport t;
  auto c = f.connect(0, t);
  CHECK(t.take() == Frames{R"(T:{"type":"welcome"})"});
  c->on_data(cmd("subscribe", kRoom1));
  CHECK(t.take() == Frames{confirm(kRoom1)});
  c->on_data(cmd("subscribe", kRoom1));
  CHECK(t.take().empty());
  // A different spelling of the same object is a different subscription.
  std::string_view respelled = R"({"room_id":1,"channel":"RoomChannel"})";
  c->on_data(cmd("subscribe", respelled));
  CHECK(t.take() == Frames{confirm(respelled)});
}

TEST_CASE("rejection runs unsubscribed and can be retried") {
  Fixture f;
  MockTransport t;
  auto c = f.connect(0, t);
  t.take();
  c->on_data(cmd("subscribe", kRoom2));
  CHECK(t.take() == Frames{reject(kRoom2)});
  CHECK(f.log.lines == std::vector<std::string>{"subscribed rejected=true", "unsubscribed rejected=true"});
  c->on_data(cmd("subscribe", kRoom2));
  CHECK(t.take() == Frames{reject(kRoom2)});
  CHECK(c->subscription_count() == 0);
}

TEST_CASE("unknown channels and malformed commands get no reply") {
  Fixture f;
  MockTransport t;
  auto c = f.connect(0, t);
  t.take();
  c->on_data(cmd("subscribe", R"({"channel":"NopeChannel"})"));
  c->on_data(cmd("subscribe", "not json"));
  c->on_data(cmd("subscribe", "[1]"));
  c->on_data(text_frame("not json"));
  c->on_data(text_frame("[]"));
  c->on_data(text_frame(R"({"command":"dance"})"));
  c->on_data(text_frame(R"({"command":"subscribe"})"));
  c->on_data(cmd("unsubscribe", kRoom1));
  c->on_data(cmd("message", kRoom1, R"({"action":"echo"})"));
  c->on_data(client_frame(2, true, false, "binary"));
  CHECK(t.take().empty());
  c->on_data(cmd("subscribe", kRoom1));
  CHECK(t.take() == Frames{confirm(kRoom1)});
}

TEST_CASE("a leading :: resolves like safe_constantize") {
  Fixture f;
  MockTransport a, b;
  auto ca = f.connect(0, a);
  auto cb = f.connect(0, b);
  a.take();
  b.take();
  std::string_view prefixed = R"({"channel":"::RoomChannel","room_id":1})";
  ca->on_data(cmd("subscribe", prefixed));
  cb->on_data(cmd("subscribe", kRoom1));
  CHECK(a.take() == Frames{confirm(prefixed)});
  CHECK(b.take() == Frames{confirm(kRoom1)});
  CHECK(f.hub.broadcast("room:room-1", json::Value(json::Value::Object{{"roomId", 1}})) == 2);
  f.hub.drain(0);
  CHECK(a.take() == Frames{message(prefixed, R"({"roomId":1})")});
  CHECK(b.take() == Frames{message(kRoom1, R"({"roomId":1})")});
  ca->on_data(cmd("subscribe", R"({"channel":"::HeartbeatChannel"})"));
  CHECK(a.take() == Frames{confirm(R"({"channel":"::HeartbeatChannel"})")});
}

TEST_CASE("broadcasts reach subscribers as escaped JSON") {
  Fixture f;
  MockTransport t;
  auto c = f.connect(0, t);
  c->on_data(cmd("subscribe", kRoom1));
  t.take();
  f.hub.broadcast("room:room-1", json::Value("<b>&</b>"));
  f.hub.drain(0);
  CHECK(t.take() == Frames{message(kRoom1, R"("\u003cb\u003e\u0026\u003c/b\u003e")")});
}

TEST_CASE("perform dispatches actions and defaults to receive") {
  Fixture f;
  MockTransport t;
  auto c = f.connect(0, t);
  c->on_data(cmd("subscribe", kRoom1));
  t.take();
  c->on_data(cmd("message", kRoom1, R"({"action":"echo","n":1})"));
  CHECK(t.take() == Frames{message(kRoom1, R"({"action":"echo","n":1})")});
  c->on_data(cmd("message", kRoom1, R"({"text":"hi"})"));
  CHECK(t.take() == Frames{message(kRoom1, R"({"received":{"text":"hi"}})")});
  c->on_data(cmd("message", kRoom1, R"({"action":"  ","text":"hi"})"));
  CHECK(t.take() == Frames{message(kRoom1, R"({"received":{"action":"  ","text":"hi"}})")});
  c->on_data(cmd("message", kRoom1, R"({"action":"dance"})"));
  c->on_data(cmd("message", kRoom1, R"({"action":"boom"})"));
  c->on_data(cmd("message", kRoom1, R"({"action":5})"));
  c->on_data(cmd("message", kRoom1, R"([1])"));
  c->on_data(cmd("message", kRoom1));
  CHECK(t.take().empty());
}

TEST_CASE("unsubscribe stops delivery silently") {
  Fixture f;
  MockTransport t;
  auto c = f.connect(0, t);
  c->on_data(cmd("subscribe", kRoom1));
  t.take();
  CHECK(f.hub.stream_count() == 2);
  c->on_data(cmd("unsubscribe", kRoom1));
  CHECK(t.take().empty());
  CHECK(f.hub.broadcast("room:room-1", json::Value::Object{}) == 0);
  CHECK(f.hub.stream_count() == 1);
}

TEST_CASE("remote disconnect closes every connection of the identifier") {
  Fixture f;
  MockTransport t1, t2, t3;
  auto c1 = f.connect(0, t1);
  auto c2 = f.connect(1, t2);
  auto c3 = f.connect(0, t3, false, "user-2");
  t1.take();
  t2.take();
  t3.take();
  c1->on_data(cmd("subscribe", kRoom1));
  t1.take();
  f.hub.broadcast_encoded("action_cable/user-1", protocol::remote_disconnect_payload(true));
  f.hub.drain(0);
  f.hub.drain(1);
  Frames expected{R"(T:{"type":"disconnect","reason":"remote","reconnect":true})", "C:1000"};
  CHECK(t1.take() == expected);
  CHECK(t2.take() == expected);
  CHECK(t3.take().empty());
  CHECK(t1.closes == 1);
  CHECK(t1.last_grace == kCloseGrace);
  // The client answers: the transport closes, and the channels unsubscribe.
  c1->on_data(client_frame(8, true, false, std::string("\x03\xe8", 2)));
  CHECK(t1.closes == 2);
  c1->on_closed();
  CHECK(f.log.lines.back() == "unsubscribed rejected=false");
  f.hub.broadcast_encoded("action_cable/user-1", protocol::remote_disconnect_payload(false));
  f.hub.drain(1);
  CHECK(t2.take().empty());  // already closing
}

TEST_CASE("remote disconnect keeps the reconnect flag and ignores other payloads") {
  Fixture f;
  MockTransport t;
  auto c = f.connect(0, t);
  t.take();
  f.hub.broadcast_encoded("action_cable/user-1", R"({"type":"other"})");
  f.hub.broadcast_encoded("action_cable/user-1", "not json");
  f.hub.drain(0);
  CHECK(t.take().empty());
  f.hub.broadcast_encoded("action_cable/user-1", R"({"type":"disconnect","reconnect":false})");
  f.hub.drain(0);
  CHECK(t.take() == Frames{R"(T:{"type":"disconnect","reason":"remote","reconnect":false})", "C:1000"});
}

TEST_CASE("restart, lag, stall and unauthorized") {
  Fixture f;
  {
    MockTransport t;
    auto c = f.connect(0, t);
    t.take();
    f.hub.restart(0);
    CHECK(t.take() == Frames{R"(T:{"type":"disconnect","reason":"server_restart","reconnect":true})", "C:1000"});
  }
  {
    MockTransport t;
    auto c = f.connect(0, t);
    t.take();
    c->on_lagged();
    CHECK(t.take() == Frames{R"(T:{"type":"disconnect","reason":null,"reconnect":true})", "C:1000"});
  }
  {
    MockTransport t;
    auto c = f.connect(0, t);
    t.take();
    c->on_write_stall();
    CHECK(t.closes == 1);
    CHECK(t.last_grace == std::chrono::milliseconds(0));
  }
  {
    MockTransport t;
    Connection c(f.hub, 0, t, f.registry, false);
    c.open(nullptr, "user-1", [] { return false; });
    CHECK(t.take() == Frames{R"(T:{"type":"disconnect","reason":"unauthorized","reconnect":false})", "C:1000"});
    CHECK(f.hub.stream_count() == 0);
  }
}

TEST_CASE("ping is sent to every connection of the worker") {
  Fixture f;
  MockTransport a, b;
  auto ca = f.connect(0, a);
  auto cb = f.connect(1, b);
  a.take();
  b.take();
  f.hub.beat(0, 1700000000);
  CHECK(a.take() == Frames{R"(T:{"type":"ping","message":1700000000})"});
  CHECK(b.take().empty());
  CHECK(protocol::kBeatIntervalSeconds == 3);
}

TEST_CASE("client close is answered with its code, protocol errors with theirs") {
  Fixture f;
  {
    MockTransport t;
    auto c = f.connect(0, t);
    t.take();
    c->on_data(client_frame(8, true, false, std::string("\x03\xe9", 2)));
    CHECK(t.take() == Frames{"C:1001"});
    CHECK(t.closes == 1);
  }
  {
    MockTransport t;
    auto c = f.connect(0, t);
    t.take();
    c->on_data(client_frame(1, true, false, "\xff\xfe"));
    CHECK(t.take() == Frames{"C:1007"});
    CHECK(t.closes == 1);
  }
  {
    MockTransport t;
    auto c = f.connect(0, t);
    t.take();
    c->on_data(client_frame(9, true, false, "pp"));
    CHECK(t.take() == Frames{"P:pp"});
  }
}

TEST_CASE("a connection holds a bounded number of subscriptions") {
  Fixture f;
  MockTransport t;
  auto c = f.connect(0, t);
  t.take();
  for (int n = 0; n < 64; ++n) {
    std::string id = R"({"channel":"HeartbeatChannel","nonce":)" + std::to_string(n) + "}";
    c->on_data(cmd("subscribe", id));
    CHECK(t.take() == Frames{confirm(id)});
  }
  c->on_data(cmd("subscribe", R"({"channel":"HeartbeatChannel","nonce":64})"));
  CHECK(t.take().empty());
  MockTransport t2;
  auto c2 = f.connect(0, t2);
  t2.take();
  c2->on_data(cmd("subscribe", R"({"channel":"HeartbeatChannel","pad":")" + std::string(5000, 'x') + R"("})"));
  CHECK(t2.take().empty());
  // Exactly 4096 bytes is allowed.
  std::string base = R"({"channel":"HeartbeatChannel","pad":")";
  std::string id = base + std::string(4096 - base.size() - 2, 'x') + R"("})";
  REQUIRE(id.size() == 4096);
  c2->on_data(cmd("subscribe", id));
  CHECK(t2.take() == Frames{confirm(id)});
}

TEST_CASE("channel names and broadcastings") {
  using namespace protocol;
  CHECK(channel_name("RoomChannel") == "room");
  CHECK(channel_name("TypingNotificationsChannel") == "typing_notifications");
  CHECK(channel_name("Turbo::StreamsChannel") == "turbo:streams");
  CHECK(channel_name("HTMLChannel") == "html");
  CHECK(channel_name("HTMLParserChannel") == "html_parser");
  std::string_view room[] = {"Z2lkOi8vY2FtcGZpcmUvUm9vbXM6Ok9wZW4vMQ"};
  CHECK(broadcasting_for("PresenceChannel", room) == "presence:Z2lkOi8vY2FtcGZpcmUvUm9vbXM6Ok9wZW4vMQ");
  std::string_view parts[] = {room[0], "messages"};
  CHECK(stream_name_from(parts) == "Z2lkOi8vY2FtcGZpcmUvUm9vbXM6Ok9wZW4vMQ:messages");
}

TEST_CASE("server frames are exact JSON bytes") {
  using namespace protocol;
  CHECK(welcome() == R"({"type":"welcome"})");
  CHECK(ping(5) == R"({"type":"ping","message":5})");
  CHECK(disconnect(DisconnectReason::InvalidRequest, "false") == R"({"type":"disconnect","reason":"invalid_request","reconnect":false})");
  CHECK(confirmation("{\"a\":1}") == R"({"identifier":"{\"a\":1}","type":"confirm_subscription"})");
  CHECK(rejection("x") == R"({"identifier":"x","type":"reject_subscription"})");
}
