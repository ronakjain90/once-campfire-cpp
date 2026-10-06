// What the autocomplete views read off a user. Rails: the locals of
// app/views/autocompletable/users/_prompt_item.html.erb. Rust: crates/views/src/autocompletable.rs (MentionUser).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace campfire::views::autocompletable {

struct MentionUserView {
  std::int64_t id = 0;
  std::string name;
  std::string title;            // `User#title`
  std::string attachable_sgid;  // `user.attachable_sgid`
  std::string avatar_url;       // `fresh_user_avatar_path(user)`
};

}  // namespace campfire::views::autocompletable
