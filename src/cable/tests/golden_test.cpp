// Replays the frames that the Rails reference sent to a client (tests/golden/reference.json).
// Rust: crates/cable/tests/golden.rs. Every frame must match byte for byte. Ping times are
// normalized. The "cross origin" and "plain http" sessions are HTTP answers of the front server
// (T5), so they are not tested here.
#include <doctest.h>

#include <fstream>
#include <map>
#include <sstream>

#include "cable/connection.hpp"
#include "cable/tests/support.hpp"
#include "compat/json.hpp"

using namespace campfire;
using namespace campfire::cable;
using namespace campfire::cable::testing;
namespace json = campfire::compat::json;

namespace {

struct User {
  std::uint64_t id;
  std::string name;
  std::uint64_t room_id;
};

std::string load(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string golden_path() {
  std::string file = __FILE__;
  return file.substr(0, file.rfind('/')) + "/golden/reference.json";
}

std::string identifier(const json::Value& v) {
  return json::generate(v);
}

std::string command(std::string_view name, const std::string& id, const std::string* data = nullptr) {
  json::Value::Object o{{"command", name}, {"identifier", id}};
  if (data != nullptr) o.emplace_back("data", *data);
  return json::generate(json::Value(std::move(o)));
}

// RoomChannel#subscribed: stream_for a room that the user belongs to, else reject.
std::optional<std::string> subscribe_to_room(Subscription& sub) {
  const json::Value* id = sub.param("room_id");
  const User& user = sub.user<User>();
  if (id == nullptr) {
    sub.reject();
    return std::nullopt;
  }
  auto n = id->to_int64();
  if (!n || static_cast<std::uint64_t>(*n) != user.room_id) {
    sub.reject();
    return std::nullopt;
  }
  std::string room = "room-" + std::to_string(*n);
  std::string_view parts[] = {room};
  sub.stream_for(parts);
  return room;
}

class RoomChannel : public Channel {
 public:
  Status subscribed(Subscription& sub) override {
    subscribe_to_room(sub);
    return {};
  }
};

class TypingChannel : public Channel {
 public:
  Status subscribed(Subscription& sub) override {
    room_ = subscribe_to_room(sub);
    return {};
  }
  Result<bool> perform(std::string_view action, const json::Value&, Subscription& sub) override {
    if (action != "start" && action != "stop") return false;
    const User& u = sub.user<User>();
    json::Value message(json::Value::Object{
        {"action", action},
        {"user",
         json::Value(json::Value::Object{{"id", json::Value(static_cast<std::int64_t>(u.id))}, {"name", u.name}})}});
    std::string_view parts[] = {*room_};
    sub.hub().broadcast(sub.broadcasting_for(parts), message);
    return true;
  }

 private:
  std::optional<std::string> room_;
};

// Turbo::StreamsChannel with the room guard: the verifier is a table of the recorded signatures.
class TurboChannel : public Channel {
 public:
  explicit TurboChannel(const std::map<std::string, std::string>& signed_names) : signed_(signed_names) {}
  Status subscribed(Subscription& sub) override {
    std::optional<std::string> name;
    if (const json::Value* s = sub.param("signed_stream_name"); s != nullptr && s->is_string()) {
      auto it = signed_.find(s->as_string());
      if (it != signed_.end()) name = it->second;
    }
    std::string_view n = name ? std::string_view(*name) : std::string_view();
    auto colon = n.find(':');
    if (colon != std::string_view::npos && n.substr(colon + 1) == "messages") {
      sub.reject();
      return {};
    }
    if (name) {
      sub.stream_from(*name);
    } else {
      sub.reject();
    }
    return {};
  }

 private:
  const std::map<std::string, std::string>& signed_;
};

struct Step {
  std::string name;
  std::vector<std::string> expected;
};

std::string normalize(const std::string& frame) {
  if (!frame.starts_with(R"(T:{"type":"ping","message":)")) return frame;
  return R"(T:{"type":"ping","message":<unix>})";
}

// Maps the recorded form to the decoded form: text frames get "T:", the close frame "C:".
std::vector<std::string> expected_frames(const json::Value& frames) {
  std::vector<std::string> out;
  for (const auto& f : frames.as_array()) {
    const std::string& s = f.as_string();
    if (s.starts_with("upgrade ")) continue;
    if (s.starts_with("close Some((")) {
      out.push_back("C:" + s.substr(12, s.find(',') - 12));
    } else if (s == "end") {
      out.push_back("end");
    } else {
      out.push_back(normalize("T:" + s));
    }
  }
  return out;
}

void replay(bool deflate) {
  auto doc = json::parse(load(golden_path()));
  REQUIRE(doc);
  const json::Value* tokens = doc->find("tokens");
  REQUIRE(tokens != nullptr);
  auto tok = [&](const char* k) { return tokens->find(k)->as_string(); };
  auto room_id = std::stoull(tok("ROOM_ID"));
  auto closed_room_id = std::stoull(tok("CLOSED_ROOM_ID"));

  std::map<std::string, std::string> signed_names{
      {tok("ROOMS_SIGNED"), "rooms"},
      {tok("ROOM_MESSAGES_SIGNED"), "Z2lkOi8vY2FtcGZpcmUvUm9vbXM6Ok9wZW4vNjk5NDQ4MzMw:messages"},
  };

  ChannelRegistry registry;
  registry.add("ApplicationCable::Channel", [] { return std::make_unique<EmptyChannel>(); });
  registry.add("HeartbeatChannel", [] { return std::make_unique<EmptyChannel>(); });
  registry.add("RoomChannel", [] { return std::make_unique<RoomChannel>(); });
  registry.add("TypingNotificationsChannel", [] { return std::make_unique<TypingChannel>(); });
  registry.add("Turbo::StreamsChannel", [&] { return std::make_unique<TurboChannel>(signed_names); });

  auto room = [&](std::uint64_t id) {
    return identifier(json::Value(
        json::Value::Object{{"channel", "RoomChannel"}, {"room_id", json::Value(static_cast<std::int64_t>(id))}}));
  };
  auto typing = identifier(json::Value(json::Value::Object{
      {"channel", "TypingNotificationsChannel"}, {"room_id", json::Value(static_cast<std::int64_t>(room_id))}}));
  auto turbo = [&](const json::Value& signed_name) {
    json::Value::Object o{{"channel", "Turbo::StreamsChannel"}};
    if (!signed_name.is_null()) o.emplace_back("signed_stream_name", signed_name);
    return identifier(json::Value(std::move(o)));
  };
  auto plain = [&](const char* channel) { return identifier(json::Value(json::Value::Object{{"channel", channel}})); };
  auto perform = [&](const json::Value::Object& data) {
    auto d = json::generate(json::Value(data));
    return command("message", typing, &d);
  };

  std::vector<std::pair<std::string, std::string>> script = {
      {"subscribe heartbeat", command("subscribe", plain("HeartbeatChannel"))},
      {"subscribe heartbeat again", command("subscribe", plain("HeartbeatChannel"))},
      {"subscribe member room", command("subscribe", room(room_id))},
      {"subscribe non-member room", command("subscribe", room(closed_room_id))},
      {"subscribe unknown channel", command("subscribe", plain("NopeChannel"))},
      {"subscribe base channel", command("subscribe", plain("ApplicationCable::Channel"))},
      {"subscribe turbo rooms", command("subscribe", turbo(json::Value(tok("ROOMS_SIGNED"))))},
      {"subscribe turbo forged", command("subscribe", turbo(json::Value("InJvb21zIg==--0000")))},
      {"subscribe turbo unsigned", command("subscribe", turbo(json::Value()))},
      {"subscribe turbo guarded room messages", command("subscribe", turbo(json::Value(tok("ROOM_MESSAGES_SIGNED"))))},
      {"subscribe typing", command("subscribe", typing)},
      {"perform typing start", perform({{"action", "start"}})},
      {"perform unknown action", perform({{"action", "dance"}})},
      {"perform default receive", perform({{"text", "hi"}})},
      {"unsubscribe typing", command("unsubscribe", typing)},
      {"perform after unsubscribe", perform({{"action", "start"}})},
      {"unknown command", R"({"command":"dance"})"},
      {"invalid json", "not json"},
  };

  const json::Value* sessions = doc->find("sessions");
  const auto& authenticated = sessions->find("authenticated")->as_array();
  REQUIRE(authenticated.size() == script.size() + 2);

  Hub hub(1, nullptr);
  MockTransport transport;
  User user{std::stoull(tok("USER_ID")), tok("USER_NAME"), room_id};
  auto owner = std::make_shared<const User>(user);
  std::string gid = "gid://campfire/User/" + tok("USER_ID");

  {
    Connection conn(hub, 0, transport, registry, deflate);
    conn.open(owner, gid);
    CHECK(transport.take() == expected_frames(*authenticated[0].find("frames")));
    int checked = 1;
    for (std::size_t i = 0; i < script.size(); ++i) {
      const auto& expected = authenticated[i + 1];
      REQUIRE(expected.find("step")->as_string() == script[i].first);
      conn.on_data(text_frame(script[i].second));
      hub.drain(0);
      INFO("step: " << script[i].first);
      CHECK(transport.take() == expected_frames(*expected.find("frames")));
      ++checked;
    }
    hub.beat(0, 1700000000);
    auto ping = transport.take();
    REQUIRE(ping.size() == 1);
    CHECK(normalize(ping[0]) == expected_frames(*authenticated.back().find("frames"))[0]);
    ++checked;
    CHECK(checked == 20);
  }
  CHECK(hub.stream_count() == 0);

  // remote disconnect
  {
    const auto& rd = sessions->find("remote disconnect")->as_array();
    Connection conn(hub, 0, transport, registry, deflate);
    conn.open(owner, gid);
    CHECK(transport.take() == expected_frames(*rd[0].find("frames")));
    conn.on_data(text_frame(command("subscribe", room(room_id))));
    CHECK(transport.take() == expected_frames(*rd[1].find("frames")));
    hub.broadcast_encoded(protocol::internal_channel(gid), protocol::remote_disconnect_payload(true));
    hub.drain(0);
    auto frames = transport.take();
    // The recorded "end" is the TCP close that the transport does.
    REQUIRE(frames.size() == 2);
    CHECK(frames[0] == expected_frames(*rd[2].find("frames"))[0]);
    CHECK(frames[1] == expected_frames(*rd[2].find("frames"))[1]);
    CHECK(transport.closes == 1);
    CHECK(conn.closing());
  }

  // unauthenticated
  {
    const auto& un = sessions->find("unauthenticated")->as_array();
    Connection conn(hub, 0, transport, registry, deflate);
    conn.reject_unauthorized();
    auto expected = expected_frames(*un[0].find("frames"));
    expected.pop_back();  // "end"
    CHECK(transport.take() == expected);
  }
}

}  // namespace

TEST_CASE("reference frames replay byte for byte") {
  replay(false);
}

TEST_CASE("reference frames replay with permessage-deflate") {
  replay(true);
}
