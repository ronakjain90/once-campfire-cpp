// Helpers of the message views. Rails: app/helpers/users_helper.rb (avatar_tag), app/helpers/rooms/refreshes_helper.rb
// is not used here; `mention_prompt_tag` is in app/helpers/messages_helper.rb. Rust: crates/views/src/helpers/users.rs,
// crates/views/src/rooms.rs (mention_prompt_src).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "views/context.hpp"
#include "views/helpers/forms.hpp"
#include "views/helpers/tag.hpp"

namespace campfire::views::messages {

// `avatar_tag(user, **options)`: the options go to the image.
void avatar_tag(Out& out, const ViewContext& ctx, std::int64_t user_id, std::string_view title,
                std::string_view avatar_url, helpers::Attrs options = {});

// `mention_prompt_tag(room)`: the `src` is `autocompletable_users_path(room_id: room.id)`.
[[nodiscard]] std::string mention_prompt_src(std::int64_t room_id);

}  // namespace campfire::views::messages
