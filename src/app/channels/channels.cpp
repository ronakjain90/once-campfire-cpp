// The Action Cable channels of the app. Rails: reference/app/channels/*.rb. Rust: crates/campfire/src/channels/*.rs.
#include "app/channels/channels.hpp"

#include <array>
#include <cmath>
#include <string>

#include "app/broadcasts.hpp"
#include "app/channels/detached.hpp"
#include "app/controllers/rooms.hpp"
#include "app/worker_state.hpp"
#include "cable/hub.hpp"
#include "compat/global_id.hpp"
#include "compat/json.hpp"
#include "compat/turbo.hpp"
#include "core/arena.hpp"
#include "core/log.hpp"
#include "models/membership_connect.hpp"
#include "models/room.hpp"

namespace campfire::app::channels {

namespace json = compat::json;
using cable::Channel;
using cable::Subscription;

WorkerContext& worker_context() noexcept {
  thread_local WorkerContext context;
  return context;
}

std::string connection_identifier(std::int64_t user_id) {
  return compat::global_id::GlobalId::make("User", std::to_string(user_id)).to_string();
}

std::string read_rooms_stream(std::int64_t user_id) {
  return "user_" + std::to_string(user_id) + "_reads";
}

std::size_t read_room(const App& app, std::int64_t user_id, std::int64_t room_id) {
  cable::Hub* hub = app.hub.load(std::memory_order_acquire);
  if (hub == nullptr) return 0;
  return hub->broadcast(read_rooms_stream(user_id),
                        json::Value(json::Value::Object{{"room_id", json::Value(room_id)}}));
}

namespace {

const CableUser& current_user(const Subscription& sub) {
  return sub.user<CableUser>();
}

// How Active Record casts a value for an integer `id` condition: numbers are cut, booleans are 1 and 0, and strings go
// through `to_i` unless they do not start like a number, which matches nothing.
std::optional<std::int64_t> cast_id(const json::Value& value) {
  if (value.is_bool()) return value.as_bool() ? 1 : 0;
  if (const auto n = value.to_int64()) return n;
  if (value.is_double()) {
    const double d = value.as_double();
    if (std::isfinite(d) && std::fabs(d) < 9.2e18) return static_cast<std::int64_t>(std::trunc(d));
    return std::nullopt;
  }
  if (const std::string* text = value.get_string()) return controllers::cast_id(*text);
  return std::nullopt;
}

// `GlobalID#to_param` of a room: its STI class names it (`gid://campfire/Rooms::Open/1`).
std::string room_gid_param(const models::Room& room) {
  return compat::global_id::GlobalId::make(room.type, std::to_string(room.id)).to_param();
}

// ---- Streams ------------------------------------------------------------------------------------------------------

// `verified_stream_name_from_params`: `params[:signed_stream_name]` verified. A missing or null name is simply
// unverified. Any other value that is not a string makes `MessageVerifier#verified` raise.
Result<std::optional<std::string>> verified_stream_name(const App& app, const Subscription& sub) {
  const json::Value* signed_name = sub.param("signed_stream_name");
  if (signed_name == nullptr || signed_name->is_null()) return std::optional<std::string>{};
  const std::string* text = signed_name->get_string();
  if (text == nullptr) {
    return std::unexpected(
        Error{Errc::Internal, "undefined method 'valid_encoding?' for " + json::encode(*signed_name)});
  }
  return compat::turbo::verified_stream_name(app.secrets, *text);
}

constexpr std::string_view kStreamSuffix = "messages";

// `RoomMessagesChannel.guarded_stream?`: true for the stream names that this channel guards, whoever asks.
bool guarded_stream(std::string_view stream_name) {
  const std::size_t colon = stream_name.find(':');
  return colon != std::string_view::npos && stream_name.substr(colon + 1) == kStreamSuffix;
}

// Constants that a GID can name and that are not rooms: `only: Room` turns them away without raising.
constexpr std::array<std::string_view, 11> kKnownModels = {
    "Account", "Ban",     "Boost", "Current", "Membership", "Message", "Push::Subscription",
    "Search",  "Session", "User",  "Webhook"};

// `GlobalID::Locator.locate gid_param, only: Room`, with `RecordNotFound` as nothing.
Result<std::optional<models::Room>> room_from(db::Connection& conn, Arena& arena, std::string_view gid_param) {
  auto gid = compat::global_id::GlobalId::parse(gid_param);
  if (!gid) gid = compat::global_id::GlobalId::from_param(gid_param);
  if (!gid) return std::optional<models::Room>{};
  std::optional<std::string_view> required_type;
  if (gid->model_name != "Room") {
    if (gid->model_name == models::kRoomOpen || gid->model_name == models::kRoomClosed ||
        gid->model_name == models::kRoomDirect) {
      required_type = gid->model_name;
    } else if (std::find(kKnownModels.begin(), kKnownModels.end(), gid->model_name) != kKnownModels.end()) {
      return std::optional<models::Room>{};
    } else {
      return std::unexpected(Error{Errc::Internal, "uninitialized constant " + gid->model_name});
    }
  }
  const auto id = controllers::cast_id(gid->id);
  if (!id) return std::optional<models::Room>{};
  auto room = models::rooms::find_by_id(conn, arena, *id);
  if (!room) return std::unexpected(room.error());
  if (*room && required_type && (*room)->type != *required_type) return std::optional<models::Room>{};
  return std::move(*room);
}

// `RoomMessagesChannel.subscribable_room(user, stream_name)`.
Result<bool> subscribable(db::Connection& conn, std::int64_t user_id, std::string_view stream_name) {
  const std::size_t colon = stream_name.find(':');
  if (colon == std::string_view::npos || stream_name.substr(colon + 1) != kStreamSuffix) return false;
  Arena arena(1024);
  auto room = room_from(conn, arena, stream_name.substr(0, colon));
  if (!room) return std::unexpected(room.error());
  if (!*room) return false;
  auto member = models::rooms::find_for_user(conn, arena, user_id, models::RoomScope::All, (*room)->id);
  if (!member) return std::unexpected(member.error());
  return member->has_value();
}

bool blank(std::string_view text) {
  return text.find_first_not_of(" \t\n\v\f\r") == std::string_view::npos;
}

// ---- Channels -----------------------------------------------------------------------------------------------------

// `RoomChannel`: streams the room of `params[:room_id]` for a member, rejects anyone else. `PresenceChannel` and
// `TypingNotificationsChannel` inherit it.
class RoomChannel : public Channel {
 public:
  Status subscribed(Subscription& sub) override { return subscribe_room(sub); }

  Result<bool> perform(std::string_view action, const json::Value&, Subscription& sub) override {
    // `subscribed` is public in Ruby, so it is an action too.
    if (action == "subscribed" && !sub.rejected()) {
      if (auto done = subscribe_room(sub); !done) return std::unexpected(done.error());
      return true;
    }
    return false;
  }

 protected:
  // `current_user.rooms.find_by(id: params[:room_id])`, then `stream_for @room` or `reject`.
  Status subscribe_room(Subscription& sub) {
    room_.reset();
    std::optional<models::Room> found;
    if (const json::Value* param = sub.param("room_id")) {
      if (const auto room_id = cast_id(*param)) {
        Arena arena(1024);
        auto room = models::rooms::find_for_user(worker_state().reader(), arena, current_user(sub).id,
                                                 models::RoomScope::All, *room_id);
        if (!room) return std::unexpected(room.error());
        found = std::move(*room);
      }
    }
    if (found) {
      const std::string param = room_gid_param(*found);
      const std::array<std::string_view, 1> parts{param};
      sub.stream_for(parts);
      room_ = std::move(found);
    } else {
      sub.reject();
    }
    return {};
  }

  std::optional<models::Room> room_;
};

// `PresenceChannel`: marks the membership connected while subscribed and tells the other windows of the user that the
// room is read.
class PresenceChannel final : public RoomChannel {
 public:
  explicit PresenceChannel(const App& app) : app_(app) {}

  Status subscribed(Subscription& sub) override {
    if (auto done = subscribe_room(sub); !done) return done;
    if (!sub.rejected()) return present(sub);
    return {};
  }

  Status unsubscribed(Subscription& sub) override {
    if (sub.rejected()) return {};
    return absent(sub);
  }

  Result<bool> perform(std::string_view action, const json::Value& data, Subscription& sub) override {
    if (sub.rejected()) return false;
    if (action == "present") {
      if (auto done = present(sub); !done) return std::unexpected(done.error());
    } else if (action == "absent") {
      if (auto done = absent(sub); !done) return std::unexpected(done.error());
    } else if (action == "refresh") {
      if (auto done = refresh(sub); !done) return std::unexpected(done.error());
    } else {
      return RoomChannel::perform(action, data, sub);
    }
    return true;
  }

 private:
  Status present(const Subscription& sub) { return write(sub, Change::Present); }
  Status absent(const Subscription& sub) { return write(sub, Change::Absent); }
  Status refresh(const Subscription& sub) { return write(sub, Change::Refresh); }

  enum class Change { Present, Absent, Refresh };

  // The membership changes through the writer. The client does not wait for it: the write runs as a task of the worker,
  // and `present` tells the user's windows when it is done.
  Status write(const Subscription& sub, Change change) {
    // `@room` is nil only after a rejection, when these callbacks do not run.
    if (!room_) return std::unexpected(Error{Errc::Internal, "undefined method 'memberships' for nil"});
    const std::int64_t room_id = room_->id;
    const std::int64_t user_id = current_user(sub).id;
    Scheduler* scheduler = worker_context().scheduler;
    if (scheduler == nullptr) return std::unexpected(Error{Errc::Internal, "the worker has no scheduler"});
    run_detached(apply(app_, *scheduler, change, room_id, user_id));
    return {};
  }

  static Task<void> apply(const App& app, Scheduler& scheduler, Change change, std::int64_t room_id,
                          std::int64_t user_id) {
    auto found = co_await app.db->write(scheduler, [&](db::Tx& tx) -> Result<bool> {
      switch (change) {
        case Change::Present: return models::memberships::present(tx, room_id, user_id);
        case Change::Absent: return models::memberships::disconnected(tx, room_id, user_id);
        case Change::Refresh: return models::memberships::refresh_connection(tx, room_id, user_id);
      }
      return false;
    });
    if (!found) {
      log_error("Could not execute command: {}", found.error().message);
    } else if (!*found) {
      log_error("Could not execute command: undefined method for nil (membership)");
    } else if (change == Change::Present) {
      read_room(app, user_id, room_id);
    }
  }

  const App& app_;
};

// `TypingNotificationsChannel`: `start` and `stop` go to everyone who streams the room.
class TypingNotificationsChannel final : public RoomChannel {
 public:
  Result<bool> perform(std::string_view action, const json::Value& data, Subscription& sub) override {
    if (sub.rejected()) return false;
    if (action == "start" || action == "stop") {
      if (auto done = broadcast(action, sub); !done) return std::unexpected(done.error());
      return true;
    }
    return RoomChannel::perform(action, data, sub);
  }

 private:
  // `broadcast_to @room, action:, user: current_user.slice(:id, :name)`.
  Status broadcast(std::string_view action, const Subscription& sub) {
    if (!room_) return std::unexpected(Error{Errc::Internal, "undefined method 'to_gid_param' for nil"});
    const CableUser& user = current_user(sub);
    json::Value::Object who{{"id", json::Value(user.id)}, {"name", json::Value(user.name)}};
    json::Value::Object payload{{"action", json::Value(action)}, {"user", json::Value(std::move(who))}};
    const std::string param = room_gid_param(*room_);
    const std::array<std::string_view, 1> parts{param};
    sub.hub().broadcast(sub.broadcasting_for(parts), json::Value(std::move(payload)));
    return {};
  }
};

// `ReadRoomsChannel` and `UnreadRoomsChannel`: the stream of the user.
class UserStreamChannel final : public Channel {
 public:
  explicit UserStreamChannel(std::string (*stream_name)(std::int64_t)) : stream_name_(stream_name) {}
  Status subscribed(Subscription& sub) override {
    sub.stream_from(stream_name_(current_user(sub).id));
    return {};
  }
  Result<bool> perform(std::string_view action, const json::Value&, Subscription& sub) override {
    if (action != "subscribed") return false;
    if (auto done = subscribed(sub); !done) return std::unexpected(done.error());
    return true;
  }

 private:
  std::string (*stream_name_)(std::int64_t);
};

// `RoomMessagesChannel`: a member of the room of the verified stream name gets the stream, anyone else is rejected.
class RoomMessagesChannel final : public Channel {
 public:
  explicit RoomMessagesChannel(const App& app) : app_(app) {}

  Status subscribed(Subscription& sub) override {
    auto stream_name = verified_stream_name(app_, sub);
    if (!stream_name) return std::unexpected(stream_name.error());
    if (!*stream_name || blank(**stream_name)) {
      sub.reject();
      return {};
    }
    auto allowed = subscribable(worker_state().reader(), current_user(sub).id, **stream_name);
    if (!allowed) return std::unexpected(allowed.error());
    if (*allowed) {
      sub.stream_from(**stream_name);
    } else {
      sub.reject();
    }
    return {};
  }

  // Its public methods: `subscribed`, and `verified_stream_name_from_params` of the Turbo module (it returns without
  // transmitting).
  Result<bool> perform(std::string_view action, const json::Value&, Subscription& sub) override {
    if (sub.rejected()) return false;
    if (action == "subscribed") {
      if (auto done = subscribed(sub); !done) return std::unexpected(done.error());
      return true;
    }
    if (action == "verified_stream_name_from_params") {
      auto name = verified_stream_name(app_, sub);
      if (!name) return std::unexpected(name.error());
      return true;
    }
    return false;
  }

 private:
  const App& app_;
};

// `Turbo::StreamsChannel` with `RoomStreamsAreAuthorized` prepended: the stream names of the room messages are turned
// away here, so that `RoomMessagesChannel` is the only door.
class TurboStreamsChannel final : public Channel {
 public:
  explicit TurboStreamsChannel(const App& app) : app_(app) {}

  Status subscribed(Subscription& sub) override {
    auto stream_name = verified_stream_name(app_, sub);
    if (!stream_name) return std::unexpected(stream_name.error());
    // The guard also sees a name that failed verification, as "" (Ruby's `nil.to_s`).
    if (guarded_stream(stream_name->value_or(""))) {
      sub.reject();
      return {};
    }
    if (*stream_name) {
      sub.stream_from(**stream_name);
    } else {
      sub.reject();
    }
    return {};
  }

 private:
  const App& app_;
};

}  // namespace

cable::ChannelRegistry make_registry(const App& app) {
  cable::ChannelRegistry registry;
  registry.add("ApplicationCable::Channel", [] { return std::make_unique<cable::EmptyChannel>(); });
  registry.add("HeartbeatChannel", [] { return std::make_unique<cable::EmptyChannel>(); });
  registry.add("PresenceChannel", [&app] { return std::make_unique<PresenceChannel>(app); });
  registry.add("ReadRoomsChannel", [] { return std::make_unique<UserStreamChannel>(&read_rooms_stream); });
  registry.add("RoomChannel", [] { return std::make_unique<RoomChannel>(); });
  registry.add("RoomMessagesChannel", [&app] { return std::make_unique<RoomMessagesChannel>(app); });
  registry.add("TypingNotificationsChannel", [] { return std::make_unique<TypingNotificationsChannel>(); });
  registry.add("UnreadRoomsChannel",
               [] { return std::make_unique<UserStreamChannel>(&broadcasts::unread_rooms_stream); });
  registry.add("Turbo::StreamsChannel", [&app] { return std::make_unique<TurboStreamsChannel>(app); });
  return registry;
}

}  // namespace campfire::app::channels
