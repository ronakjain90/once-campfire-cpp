// The Action Cable channels of the app. Rails: reference/app/channels/*.rb, concerns/room_streams_are_authorized.rb,
// Turbo::StreamsChannel. Rust: crates/campfire/src/channels/*.rs, crates/cable/src/turbo.rs.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "app/app.hpp"
#include "cable/channel.hpp"
#include "core/scheduler.hpp"

namespace campfire::app::channels {

class DetachedRunner;

// `identified_by :current_user`: the user as loaded when the connection opened.
struct CableUser {
  std::int64_t id = 0;
  std::string name;
};

// What a channel callback needs from the thread it runs on. A worker sets it when it accepts its first socket.
struct WorkerContext {
  Scheduler* scheduler = nullptr;
  unsigned index = 0;
};
[[nodiscard]] WorkerContext& worker_context() noexcept;

// `connection_gid`: the connection identifier of a user, which `remote_connections.where(current_user:)` matches.
[[nodiscard]] std::string connection_identifier(std::int64_t user_id);

// The channels under their Ruby class names. `app` and `runner` must outlive the registry.
[[nodiscard]] cable::ChannelRegistry make_registry(const App& app, DetachedRunner& runner);

// `ReadRoomsChannel.stream_name_for`: `user_<id>_reads`.
[[nodiscard]] std::string read_rooms_stream(std::int64_t user_id);
// `ActionCable.server.broadcast "user_#{id}_reads", { room_id: }`.
std::size_t read_room(const App& app, std::int64_t user_id, std::int64_t room_id);

}  // namespace campfire::app::channels
