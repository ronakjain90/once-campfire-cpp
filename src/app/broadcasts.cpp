// Rails: Turbo::Streams::Broadcasts, app/models/message/broadcasts.rb. Rust:
// crates/campfire/src/channels/broadcasts.rs, crates/cable/src/turbo.rs.
#include "app/broadcasts.hpp"

#include <array>

#include "cable/hub.hpp"
#include "compat/global_id.hpp"
#include "compat/json.hpp"
#include "core/out.hpp"
#include "views/helpers/turbo.hpp"

namespace campfire::app::broadcasts {

namespace {

std::string_view action_name(Action action) {
  switch (action) {
    case Action::Append: return "append";
    case Action::Prepend: return "prepend";
    case Action::Replace: return "replace";
    case Action::Update: return "update";
    case Action::Remove: return "remove";
    case Action::Before: return "before";
    case Action::After: return "after";
    case Action::Refresh: return "refresh";
  }
  return "";
}

void push_attribute(Out& out, std::string_view name, std::string_view value) {
  out.append_raw(" ");
  out.append_raw(name);
  out.append_raw("=\"");
  html_escape(out, value);  // ERB::Util.unwrapped_html_escape
  out.append_raw("\"");
}

constexpr Attribute kMaintainScroll{"maintain_scroll", "true"};

// Sends a Turbo Stream frame on a stream: to the hub, or else to the `turbo_broadcast` function.
std::size_t deliver(const App& app, const std::string& stream, std::string_view tag) {
  if (cable::Hub* hub = app.hub.load(std::memory_order_acquire); hub != nullptr) {
    return hub->broadcast(stream, compat::json::Value(tag));
  }
  if (app.turbo_broadcast) app.turbo_broadcast(stream, tag);
  return 0;
}

void send(const App& app, std::span<const std::string_view> streamables, Action action, std::string_view target,
          std::optional<std::string_view> html) {
  const std::string stream = stream_name(streamables);
  if (stream.empty()) return;
  deliver(app, stream, action_tag(action, target, html));
}

std::string room_param_key(const models::Room& room) {
  if (room.is_open()) return "rooms_open";
  if (room.is_closed()) return "rooms_closed";
  return "rooms_direct";
}

}  // namespace

std::string action_tag(Action action, std::string_view target, std::optional<std::string_view> html,
                       std::span<const Attribute> attributes) {
  Out out;
  out.append_raw("<turbo-stream");
  for (const Attribute& attribute : attributes) {
    if (attribute.value) push_attribute(out, attribute.name, *attribute.value);
  }
  push_attribute(out, "action", action_name(action));
  push_attribute(out, "target", target);
  out.append_raw(">");
  if (action != Action::Remove && action != Action::Refresh) {
    out.append_raw("<template>");
    out.append_raw(html.value_or(std::string_view{}));
    out.append_raw("</template>");
  }
  out.append_raw("</turbo-stream>");
  return out.to_string();
}

std::string room_messages_stream(const models::RoomRef& room) {
  return compat::global_id::GlobalId::make(room.type, std::to_string(room.id)).to_param() + ":messages";
}

std::string unread_rooms_stream(std::int64_t user_id) {
  return "user_" + std::to_string(user_id) + "_unreads";
}

std::string room_dom_id(const models::RoomRef& room, std::string_view prefix) {
  return views::helpers::dom_id(room.param_key(), std::to_string(room.id), prefix);
}

std::string room_dom_id(const models::Room& room, std::string_view prefix) {
  return views::helpers::dom_id(room_param_key(room), std::to_string(room.id), prefix);
}

std::string stream_name(std::span<const std::string_view> streamables) {
  std::string name;
  for (const std::string_view part : streamables) {
    if (part.find_first_not_of(" \t\n\v\f\r") == std::string_view::npos) continue;
    if (!name.empty()) name += ':';
    name += part;
  }
  return name;
}

std::string user_rooms_stream(std::int64_t user_id) {
  const std::string gid = compat::global_id::GlobalId::make("User", std::to_string(user_id)).to_param();
  const std::array<std::string_view, 2> parts{gid, "rooms"};
  return stream_name(parts);
}

std::size_t to_room_messages(const App& app, const models::RoomRef& room, std::string_view tag) {
  return deliver(app, room_messages_stream(room), tag);
}

void message_append(const App& app, const models::RoomRef& room, std::string_view message_html) {
  to_room_messages(app, room, action_tag(Action::Append, room_dom_id(room, "messages"), message_html));
}

void unread_room(const App& app, const models::RoomRef& room, std::span<const std::int64_t> member_ids) {
  cable::Hub* hub = app.hub.load(std::memory_order_acquire);
  if (hub == nullptr) return;
  for (const std::int64_t user_id : member_ids) {
    hub->broadcast(unread_rooms_stream(user_id),
                   compat::json::Value(compat::json::Value::Object{{"roomId", compat::json::Value(room.id)}}));
  }
}

void message_remove(const App& app, const models::RoomRef& room, std::string_view client_message_id) {
  to_room_messages(app, room, action_tag(Action::Remove, "message_" + std::string(client_message_id), std::nullopt));
}

void message_replace_presentation(const App& app, const models::RoomRef& room, std::string_view client_message_id,
                                  std::string_view presentation_html) {
  const Attribute attributes[] = {kMaintainScroll};
  to_room_messages(app, room,
                   action_tag(Action::Replace, "presentation_message_" + std::string(client_message_id),
                              presentation_html, attributes));
}

void boost_append(const App& app, const models::RoomRef& room, std::string_view client_message_id,
                  std::string_view boost_html) {
  const Attribute attributes[] = {kMaintainScroll};
  to_room_messages(
      app, room,
      action_tag(Action::Append, "boosts_message_" + std::string(client_message_id), boost_html, attributes));
}

void boost_remove(const App& app, const models::RoomRef& room, std::int64_t boost_id) {
  to_room_messages(app, room, action_tag(Action::Remove, "boost_" + std::to_string(boost_id), std::nullopt));
}

// The room controllers.

void room_remove(const App& app, const models::Room& room) {
  const std::array<std::string_view, 1> streams{"rooms"};
  send(app, streams, Action::Remove, room_dom_id(room, "list"), std::nullopt);
}

void open_room_create(const App& app, std::string_view shared_html) {
  const std::array<std::string_view, 1> streams{"rooms"};
  send(app, streams, Action::Prepend, "shared_rooms", shared_html);
}

void open_room_update(const App& app, const models::Room& room, std::string_view shared_html) {
  const std::array<std::string_view, 1> streams{"rooms"};
  send(app, streams, Action::Replace, room_dom_id(room, "list"), shared_html);
}

void closed_room_create(const App& app, std::span<const std::int64_t> member_ids, std::string_view shared_html) {
  for (const std::int64_t user_id : member_ids) {
    const std::string stream = user_rooms_stream(user_id);
    const std::array<std::string_view, 1> streams{stream};
    send(app, streams, Action::Prepend, "shared_rooms", shared_html);
  }
}

void closed_room_update(const App& app, const models::Room& room, std::span<const std::int64_t> member_ids,
                        std::string_view shared_html) {
  const std::string target = room_dom_id(room, "list");
  for (const std::int64_t user_id : member_ids) {
    const std::string stream = user_rooms_stream(user_id);
    const std::array<std::string_view, 1> streams{stream};
    send(app, streams, Action::Replace, target, shared_html);
  }
}

void direct_room_create(const App& app, std::int64_t user_id, std::string_view direct_html) {
  const std::string stream = user_rooms_stream(user_id);
  const std::array<std::string_view, 1> streams{stream};
  send(app, streams, Action::Prepend, "direct_rooms", direct_html);
}

void involvement_remove(const App& app, const models::Room& room, std::int64_t user_id) {
  const std::string stream = user_rooms_stream(user_id);
  const std::array<std::string_view, 1> streams{stream};
  send(app, streams, Action::Remove, room_dom_id(room, "list"), std::nullopt);
}

void involvement_prepend(const App& app, std::int64_t user_id, std::string_view shared_html) {
  const std::string stream = user_rooms_stream(user_id);
  const std::array<std::string_view, 1> streams{stream};
  send(app, streams, Action::Prepend, "shared_rooms", shared_html);
}

}  // namespace campfire::app::broadcasts
