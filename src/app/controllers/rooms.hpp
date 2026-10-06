// RoomsController and its subclasses. Rails: app/controllers/rooms_controller.rb, app/controllers/rooms/*.rb,
// app/controllers/concerns/tracked_room_visit.rb. Rust: crates/campfire/src/controllers/rooms.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "app/flow.hpp"
#include "app/rq.hpp"
#include "core/task.hpp"
#include "models/room.hpp"

namespace campfire::app::controllers {

// ActiveModel integer cast of a param: `find_by(id: "12abc")` finds 12, `find_by(id: "abc")` finds nothing.
[[nodiscard]] std::optional<std::int64_t> cast_id(std::string_view text);

// `set_room`: the room of `params[:room_id] || params[:id]` within `scope`, or the redirect to the root with the
// alert "Room not found or inaccessible". The result holds the room, or a halt.
[[nodiscard]] Flow<models::Room> set_room(Rq& rq, models::RoomScope scope);

// `remember_last_room_visited`: `cookies.permanent[:last_room] = @room.id`.
void remember_last_room_visited(Rq& rq, const models::Room& room);

[[nodiscard]] Task<Flow<net::Response>> rooms_index(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> opens_show(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> closeds_show(Rq& rq);

}  // namespace campfire::app::controllers
