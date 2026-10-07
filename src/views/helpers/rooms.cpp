// The parts of RoomsHelper that the sidebar and the profile use. Rust: crates/views/src/helpers/rooms.rs.
#include "views/helpers/rooms.hpp"

#include <array>

#include "routes/query.hpp"
#include "views/helpers/assets.hpp"
#include "views/helpers/forms.hpp"

namespace campfire::views::helpers {

void button_to_delete_room(Out& out, const ViewContext& ctx, std::int64_t room_id, std::string_view display_name) {
  const std::string url = ctx.url(campfire::routes::room(room_id));
  const Attrs options =
      attrs()
          .method("delete")
          .cls("btn btn--negative max-width")
          .aria("label", "Delete " + std::string(display_name))
          .data("turbo_confirm",
                "Are you sure you want to delete this room and all messages in it? This can\u2019t be undone.");
  button_to(out, url, options, [&](Out& o) {
    image_tag(o, ctx, "trash.svg", attrs().aria_hidden().size(20));
    content_tag_text(o, "span", attrs().cls("overflow-ellipsis"), display_name);
  });
}

std::string rooms_directs_with_user(std::int64_t user_id) {
  const std::array<std::int64_t, 1> ids{user_id};
  return campfire::routes::rooms_directs_with_users(ids);
}

}  // namespace campfire::views::helpers
