// Rails: Turbo::Streams::Broadcasts, app/models/message/broadcasts.rb. Rust:
// crates/campfire/src/channels/broadcasts.rs, crates/cable/src/turbo.rs.
#include "app/broadcasts.hpp"

#include "cable/hub.hpp"
#include "compat/global_id.hpp"
#include "compat/json.hpp"
#include "core/out.hpp"

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
  std::string id;
  if (!prefix.empty()) {
    id.append(prefix);
    id.push_back('_');
  }
  id += room.param_key();
  id.push_back('_');
  id += std::to_string(room.id);
  return id;
}

std::size_t to_room_messages(const App& app, const models::RoomRef& room, std::string_view tag) {
  cable::Hub* hub = app.hub.load(std::memory_order_acquire);
  if (hub == nullptr) return 0;
  return hub->broadcast(room_messages_stream(room), compat::json::Value(tag));
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

}  // namespace campfire::app::broadcasts
