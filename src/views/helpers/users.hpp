// UsersHelper and Users::SidebarHelper. Rails: app/helpers/users/sidebar_helper.rb. Rust:
// crates/views/src/helpers/users.rs.
#pragma once

#include <optional>
#include <string_view>

#include "views/helpers/tag.hpp"

namespace campfire::views::helpers {

// `sidebar_turbo_frame_tag(src:) { content }`.
void sidebar_turbo_frame_tag(Out& out, std::optional<std::string_view> src, SafeHtml content = SafeHtml::trusted(""));

}  // namespace campfire::views::helpers
