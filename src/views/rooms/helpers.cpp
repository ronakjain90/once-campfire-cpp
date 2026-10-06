// Block helpers and small helpers of the room templates. Rust: crates/views/src/rooms.rs.
#include "views/rooms/helpers.hpp"

#include "views/templates.gen.hpp"

namespace campfire::views::helpers {

void room_form(Out& out, const ViewContext& ctx, const FormRoom& room, bool can_administer, RoomKind kind,
               const std::function<void(Out&)>& body) {
  Out inner;
  body(inner);
  const std::string content = inner.to_string();
  views::rooms::layouts::form(out, ctx, room, can_administer, kind, content);
}

std::string lowercase(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

}  // namespace campfire::views::helpers
