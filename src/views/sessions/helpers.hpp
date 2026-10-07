// TranslationsHelper and the account logo helper that the sign-in page uses.
// Rails: app/helpers/translations_helper.rb, app/helpers/users_helper.rb (account_logo_tag).
// Rust: crates/views/src/helpers/translations.rs, users.rs.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "views/context.hpp"
#include "views/helpers/forms.hpp"
#include "views/helpers/tag.hpp"

namespace campfire::views {

// `User.administrator.first` for `accounts/_help_contact`.
struct HelpContact {
  std::string name;
  std::string email_address;
};

// `AllowBrowser::VERSIONS` without the browsers that it blocks outright (`ie: false`). Ruby prints the versions 17.2
// and 120 as "17.2" and "120".
struct AllowedBrowser {
  std::string_view name;
  std::string_view title;  // `browser.capitalize`
  std::string_view version;
};
inline constexpr std::array<AllowedBrowser, 4> kAllowBrowserVersions = {{
    {"safari", "Safari", "17.2"},
    {"chrome", "Chrome", "120"},
    {"firefox", "Firefox", "121"},
    {"opera", "Opera", "104"},
}};

}  // namespace campfire::views

namespace campfire::views::helpers {

// A `form_with` with no block: the opening tag and the hidden fields, and no `</form>`.
inline void open_form(Out& out, const FormWith& form) {
  form.open(out);
}

// `translations_for(key)`: the `dl` of the language list.
void translations_for(Out& out, std::string_view key);
// `translation_button(key)`: the `details` popup beside a form field.
void translation_button(Out& out, const ViewContext& ctx, std::string_view key);
// `account_logo_tag(style:)`. A nil style leaves a trailing space in the class.
void account_logo_tag(Out& out, const ViewContext& ctx, std::optional<std::string_view> style);

}  // namespace campfire::views::helpers
