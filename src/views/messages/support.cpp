// Rails: app/helpers/users_helper.rb (avatar_tag), app/helpers/messages_helper.rb. Rust:
// crates/views/src/helpers/users.rs.
#include "views/messages/support.hpp"

#include "routes/routes.hpp"
#include "views/helpers/assets.hpp"
#include "views/helpers/links.hpp"

namespace campfire::views::messages {

void avatar_tag(Out& out, const ViewContext& ctx, std::int64_t user_id, std::string_view title,
                std::string_view avatar_url, helpers::Attrs options) {
  using namespace helpers;
  const std::string path = campfire::routes::user(user_id);
  link_to(out, path, attrs().title(title).cls("btn avatar").data("turbo_frame", "_top"), [&](Out& body) {
    image_tag(body, ctx, avatar_url, attrs().aria_hidden().size(48).merge(std::move(options)));
  });
}

std::string mention_prompt_src(std::int64_t room_id) {
  return std::string(campfire::routes::autocompletable_users()) + "?room_id=" + std::to_string(room_id);
}

}  // namespace campfire::views::messages
