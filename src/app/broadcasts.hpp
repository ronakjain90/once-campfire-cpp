// The Turbo Stream broadcasts of the room controllers. Rails: `broadcast_*_to` of Turbo::Broadcastable and
// Turbo::StreamsChannel. Rust: crates/campfire/src/channels/broadcasts.rs, crates/cable/src/turbo.rs.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "app/app.hpp"
#include "models/membership.hpp"
#include "models/room.hpp"

namespace campfire::app::broadcasts {

enum class Action : std::uint8_t { Append, Prepend, Replace, Update, Remove };

// `<turbo-stream action=".." target=".."><template>..</template></turbo-stream>`. A remove has no template.
[[nodiscard]] std::string action_tag(Action action, std::string_view target, std::optional<std::string_view> html);

// The stream name of the streamables: the parts joined with ":", blank ones dropped.
[[nodiscard]] std::string stream_name(std::span<const std::string_view> streamables);

// `dom_id(room, prefix)`: "list_rooms_open_1".
[[nodiscard]] std::string room_dom_id(const models::Room& room, std::string_view prefix);

// `[ user, :rooms ]` as the streamables of a user: the GlobalID param and "rooms".
[[nodiscard]] std::string user_rooms_stream(std::int64_t user_id);

// `broadcast_remove_to :rooms, target: [ @room, :list ]` (RoomsController#destroy).
void room_remove(const App& app, const models::Room& room);
// `broadcast_prepend_to :rooms, target: :shared_rooms` (Rooms::OpensController#create).
void open_room_create(const App& app, std::string_view shared_html);
// `broadcast_replace_to :rooms, target: [ @room, :list ]` (Rooms::OpensController#update).
void open_room_update(const App& app, const models::Room& room, std::string_view shared_html);
// `broadcast_prepend_to user, :rooms, target: :shared_rooms` for each member (Rooms::ClosedsController#create).
void closed_room_create(const App& app, std::span<const std::int64_t> member_ids, std::string_view shared_html);
// `broadcast_replace_to user, :rooms, target: [ @room, :list ]` for each member (Rooms::ClosedsController#update).
void closed_room_update(const App& app, const models::Room& room, std::span<const std::int64_t> member_ids,
                        std::string_view shared_html);
// `membership.broadcast_prepend_to membership.user, :rooms, target: :direct_rooms` (Rooms::DirectsController#create).
void direct_room_create(const App& app, std::int64_t user_id, std::string_view direct_html);
// `broadcast_visibility_changes` of Rooms::InvolvementsController, for a shared room.
void involvement_remove(const App& app, const models::Room& room, std::int64_t user_id);
void involvement_prepend(const App& app, std::int64_t user_id, std::string_view shared_html);

}  // namespace campfire::app::broadcasts
