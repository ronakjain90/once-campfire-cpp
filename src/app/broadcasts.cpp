// The Turbo Stream broadcasts of the room controllers. Rust: crates/campfire/src/channels/broadcasts.rs.
#include "app/broadcasts.hpp"

#include <array>

#include "compat/global_id.hpp"
#include "core/html.hpp"
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
  }
  return {};
}

void send(const App& app, std::span<const std::string_view> streamables, Action action, std::string_view target,
          std::optional<std::string_view> html) {
  if (!app.turbo_broadcast) return;
  const std::string stream = stream_name(streamables);
  if (stream.empty()) return;
  app.turbo_broadcast(stream, action_tag(action, target, html));
}

std::string param_key(const models::Room& room) {
  if (room.is_open()) return "rooms_open";
  if (room.is_closed()) return "rooms_closed";
  return "rooms_direct";
}

}  // namespace

std::string action_tag(Action action, std::string_view target, std::optional<std::string_view> html) {
  Out out;
  out.append_raw("<turbo-stream action=\"");
  html_escape(out, action_name(action));
  out.append_raw("\" target=\"");
  html_escape(out, target);
  out.append_raw("\">");
  if (action != Action::Remove) {
    out.append_raw("<template>");
    out.append_raw(html.value_or(""));
    out.append_raw("</template>");
  }
  out.append_raw("</turbo-stream>");
  return out.to_string();
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

std::string room_dom_id(const models::Room& room, std::string_view prefix) {
  return views::helpers::dom_id(param_key(room), std::to_string(room.id), prefix);
}

std::string user_rooms_stream(std::int64_t user_id) {
  const std::string gid = compat::global_id::GlobalId::make("User", std::to_string(user_id)).to_param();
  const std::array<std::string_view, 2> parts{gid, "rooms"};
  return stream_name(parts);
}

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
