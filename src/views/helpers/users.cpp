// Users::SidebarHelper. Rails: app/helpers/users/sidebar_helper.rb. Rust: crates/views/src/helpers/users.rs.
#include "views/helpers/users.hpp"

#include "views/helpers/turbo.hpp"

namespace campfire::views::helpers {

void sidebar_turbo_frame_tag(Out& out, std::optional<std::string_view> src, SafeHtml content) {
  Attrs attributes =
      attrs()
          .data("turbo_permanent", true)
          .data("controller", "rooms-list read-rooms turbo-frame")
          .data("rooms_list_unread_class", "unread")
          // `.html_safe` in the reference: otherwise "->" is escaped.
          .data("action", SafeHtml::trusted("presence:present@window->rooms-list#read read-rooms:read->rooms-list#read "
                                            "turbo:frame-load->rooms-list#loaded refresh-room:visible@window->turbo-frame#reload"));
  if (src) attributes.set("src", Value(*src));
  attributes.set("target", Value("_top"));
  turbo_frame_tag(out, "user_sidebar", std::move(attributes), [&](Out& body) { body.append(content); });
}

}  // namespace campfire::views::helpers
