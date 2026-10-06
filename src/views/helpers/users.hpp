// UsersHelper and string helpers that the room views use (reference/app/helpers/users_helper.rb,
// ActiveSupport String and Array). Rust: crates/views/src/helpers/users.rs, helpers/application.rs.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "views/helpers/tag.hpp"
#include "views/helpers/turbo.hpp"

namespace campfire::views::helpers {

// `String#capitalize`: the first character in upper case, the rest in lower case (ASCII and Latin letters).
[[nodiscard]] std::string capitalize(std::string_view text);
// `Array#to_sentence(two_words_connector:)` with the default English connectors otherwise.
[[nodiscard]] std::string to_sentence(const std::vector<std::string>& items, std::string_view two_words_connector);
// The first character of `text` (a whole UTF-8 sequence): `str[0]`.
[[nodiscard]] std::string first_character(std::string_view text);
// `User#initials`: the first letter or digit of each word.
[[nodiscard]] std::string initials(std::string_view name);

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
