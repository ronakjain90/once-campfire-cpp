// UsersHelper and string helpers that the room views use (reference/app/helpers/users_helper.rb,
// ActiveSupport String and Array). Rust: crates/views/src/helpers/users.rs, helpers/application.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "views/context.hpp"
#include "views/helpers/assets.hpp"
#include "views/helpers/tag.hpp"
#include "views/helpers/turbo.hpp"

namespace campfire::views::helpers {

// `String#capitalize`: the first character in upper case, the rest in lower case (ASCII and Latin letters).
[[nodiscard]] std::string capitalize(std::string_view text);
// `Array#to_sentence(two_words_connector:)` with the default English connectors otherwise.
[[nodiscard]] std::string to_sentence(const std::vector<std::string>& items, std::string_view two_words_connector);
// The first character of `text` (a whole UTF-8 sequence): `str[0]`.
[[nodiscard]] std::string first_character(std::string_view text);
// `User#title`: the name and the bio joined by " – ", leaving out a blank part.
[[nodiscard]] std::string user_title(std::string_view name, const std::optional<std::string>& bio);
// `User#initials`: the first letter or digit of each word.
[[nodiscard]] std::string initials(std::string_view name);

// `avatar_tag(user, **options)`: the image in a link to the user. The options go to the image.
void avatar_tag(Out& out, const ViewContext& ctx, std::int64_t user_id, std::string_view title,
                std::string_view avatar_path, Attrs options = {});
// `user_filter_menu_tag { content }`.
template <BodyFn Body>
void user_filter_menu_tag(Out& out, Body&& body) {
  const Attrs options = attrs()
                            .cls("flex flex-column gap margin-none pad overflow-y constrain-height")
                            .data("controller", "filter")
                            .data("filter_active_class", "filter--active")
                            .data("filter_selected_class", "selected");
  content_tag(out, "menu", options, std::forward<Body>(body));
}
// `user_filter_search_tag`.
void user_filter_search_tag(Out& out);

// The attributes of `sidebar_turbo_frame_tag(src:)`: the data attributes, then `id`, `src` and `target`.
[[nodiscard]] Attrs sidebar_turbo_frame_options(std::optional<std::string_view> src);

// `sidebar_turbo_frame_tag(src:) do ... end`.
template <BodyFn Body>
void sidebar_turbo_frame_tag(Out& out, std::optional<std::string_view> src, Body&& body) {
  content_tag(out, "turbo-frame", sidebar_turbo_frame_options(src), std::forward<Body>(body));
}

// `sidebar_turbo_frame_tag(src:) { content }` with content that is already safe HTML (the welcome page).
void sidebar_turbo_frame_tag(Out& out, std::optional<std::string_view> src, SafeHtml content = SafeHtml::trusted(""));

}  // namespace campfire::views::helpers
