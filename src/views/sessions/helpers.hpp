// TranslationsHelper and the account logo helper that the sign-in page uses.
// Rails: app/helpers/translations_helper.rb, app/helpers/users_helper.rb (account_logo_tag).
// Rust: crates/views/src/helpers/translations.rs, users.rs.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "views/context.hpp"
#include "views/helpers/tag.hpp"

namespace campfire::views {

// `User.administrator.first` for `accounts/_help_contact`.
struct HelpContact {
  std::string name;
  std::string email_address;
};

}  // namespace campfire::views

namespace campfire::views::helpers {

// `translations_for(key)`: the `dl` of the language list.
void translations_for(Out& out, std::string_view key);
// `translation_button(key)`: the `details` popup beside a form field.
void translation_button(Out& out, const ViewContext& ctx, std::string_view key);
// `account_logo_tag(style:)`. A nil style leaves a trailing space in the class.
void account_logo_tag(Out& out, const ViewContext& ctx, std::optional<std::string_view> style);

}  // namespace campfire::views::helpers
