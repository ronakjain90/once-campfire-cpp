// RoomsController and its subclasses. Rails: app/controllers/rooms_controller.rb, app/controllers/rooms/*.rb,
// app/controllers/concerns/tracked_room_visit.rb. Rust: crates/campfire/src/controllers/rooms.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

// The implicit render of an action whose only template is HTML: a request that does not accept HTML is a 406.
[[nodiscard]] Flow<void> ensure_html(Rq& rq);
// `ensure_can_administer`: `head :forbidden unless Current.user.can_administer?(@room)`.
[[nodiscard]] Flow<void> ensure_can_administer(Rq& rq, const models::Room& room);
// `ensure_permission_to_create_rooms`.
[[nodiscard]] Flow<void> ensure_permission_to_create_rooms(Rq& rq);
// `params.require(:room).permit(:name)`: a value when the name was submitted (a null inside is a blank name).
[[nodiscard]] Flow<std::optional<std::optional<std::string>>> room_name_param(Rq& rq);
// `params.fetch(:user_ids, [])` as ids that `User.where(id:)` can match.
[[nodiscard]] std::vector<std::int64_t> user_ids_param(Rq& rq);
// `users/sidebars/rooms/_shared` for a room, rendered outside a layout.
[[nodiscard]] std::string shared_room_html(const models::Room& room);
// `redirect_to room_url(room)`.
[[nodiscard]] Flow<net::Response> redirect_to_room(Rq& rq, std::int64_t room_id);
// `redirect_to root_url`.
[[nodiscard]] Flow<net::Response> redirect_to_root(Rq& rq);
// `destroy_room` of RoomsController and Rooms::DirectsController: the room, then the broadcast and the redirect.
[[nodiscard]] Task<Flow<net::Response>> destroy_room(Rq& rq, models::Room room);

[[nodiscard]] Task<Flow<net::Response>> rooms_index(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> rooms_show(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> rooms_destroy(Rq& rq);
// `GET /rooms/:room_id/settings`: the route has no controller (`uninitialized constant`): a 500.
[[nodiscard]] Task<Flow<net::Response>> missing_controller(Rq& rq);
// `destroy` that Opens and Closeds inherit without `set_room`: `nil.destroy` raises.
[[nodiscard]] Task<Flow<net::Response>> destroy_without_room(Rq& rq);

[[nodiscard]] Task<Flow<net::Response>> opens_show(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> opens_new(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> opens_create(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> opens_edit(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> opens_update(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> closeds_show(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> closeds_new(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> closeds_create(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> closeds_edit(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> closeds_update(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> directs_show(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> directs_new(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> directs_create(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> directs_edit(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> directs_destroy(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> involvements_show(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> involvements_update(Rq& rq);
[[nodiscard]] Task<Flow<net::Response>> refreshes_show(Rq& rq);

}  // namespace campfire::app::controllers
