// ApplicationHelper, CableHelper, VersionHelper and ClipboardHelper parts that the layouts use
// (reference/app/helpers/*.rb). Rust: crates/views/src/helpers/application.rs.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "views/context.hpp"
#include "views/helpers/tag.hpp"

namespace campfire::views::helpers {

// `page_title_tag`: `@page_title || "Campfire"`.
void page_title_tag(Out& out, std::optional<std::string_view> page_title);
// `current_user_meta_tags`: nothing without a user.
void current_user_meta_tags(Out& out, const ViewContext& ctx);
// `script_aware_action_cable_meta_tag`.
void script_aware_action_cable_meta_tag(Out& out, const ViewContext& ctx);
// `custom_styles_tag`: the CSS of the account, not escaped (the Rails helper writes it raw).
void custom_styles_tag(Out& out, const ViewContext& ctx);
// `[ @body_class, admin_body_class, account_logo_body_class ].compact.join(" ")`.
[[nodiscard]] std::string body_classes(const ViewContext& ctx, std::optional<std::string_view> body_class);

// `link_back_to(destination)`.
void link_back_to(Out& out, const ViewContext& ctx, std::string_view destination);
// `link_back`: to the referrer, unless it is missing or the current page.
void link_back(Out& out, const ViewContext& ctx);
// `link_back_to_last_room_visited`.
void link_back_to_last_room_visited(Out& out, const ViewContext& ctx);
// `version_badge`.
void version_badge(Out& out, const ViewContext& ctx);

}  // namespace campfire::views::helpers
