// The parts of RoomsHelper that the sidebar and the profile use (reference/app/helpers/rooms/*.rb).
// Rust: crates/views/src/helpers/rooms.rs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "routes/routes.hpp"
#include "views/context.hpp"
#include "views/helpers/links.hpp"
#include "views/helpers/tag.hpp"

namespace campfire::views::helpers {

// `link_to_room(room, **attributes) { content }`: the default `data-*` entries come before the caller's.
template <BodyFn Body>
void link_to_room(Out& out, std::int64_t room_id, const Attrs& options, Body&& body) {
  const std::string id = std::to_string(room_id);
  Attrs defaults;
  defaults.set("data-rooms-list-target", Value("room"));
  defaults.set("data-room-id", Value(id));
  defaults.set("data-badge-dot-target", Value("unread"));
  defaults.set("data-sorted-list-target", Value("item"));
  link_to(out, campfire::routes::room(room_id), options.with_default_data(defaults), std::forward<Body>(body));
}

// `button_to_delete_room(room)`.
void button_to_delete_room(Out& out, const ViewContext& ctx, std::int64_t room_id, std::string_view display_name);

// `rooms_directs_path(user_ids: [ user.id ])`.
[[nodiscard]] std::string rooms_directs_with_user(std::int64_t user_id);

}  // namespace campfire::views::helpers
