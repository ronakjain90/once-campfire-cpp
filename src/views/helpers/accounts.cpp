// Rails: app/helpers/qr_code_helper.rb, users/avatars_helper.rb, users/profiles_helper.rb. Rust: crates/views/src/helpers/users.rs.
#include "views/helpers/accounts.hpp"

#include <array>

#include "compat/base64.hpp"
#include "routes/routes.hpp"
#include "views/helpers/forms.hpp"
#include "views/templates.gen.hpp"

namespace campfire::views::helpers {

namespace {

// `Users::AvatarsHelper::AVATAR_COLORS`
constexpr std::array<std::string_view, 18> kAvatarColors = {
    "#AF2E1B", "#CC6324", "#3B4B59", "#BFA07A", "#ED8008", "#ED3F1C", "#BF1B1B", "#736B1E", "#D07B53",
    "#736356", "#AD1D1D", "#BF7C2A", "#C09C6F", "#698F9C", "#7C956B", "#5D618F", "#3B3633", "#67695E"};

// `Zlib.crc32`: the CRC-32 of the IEEE standard, with a table made at first use.
std::uint32_t crc32(std::string_view data) {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const char ch : data) crc = table[(crc ^ static_cast<unsigned char>(ch)) & 0xFFU] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFU;
}

}  // namespace

std::string_view avatar_background_color(std::int64_t user_id) {
  return kAvatarColors[crc32(std::to_string(user_id)) % kAvatarColors.size()];
}

std::string qr_code_path_for(std::string_view url) {
  return campfire::routes::qr_code(compat::base64::urlsafe_encode_padded(url));
}

void profile_form_submit_button(Out& out, const ViewContext& ctx) {
  content_tag(out, "button", attrs().cls("btn btn--reversed center txt-large").type("submit"), [&](Out& o) {
    image_tag(o, ctx, "check.svg", attrs().aria_hidden().size(20));
    content_tag_text(o, "span", attrs().cls("for-screen-reader"), "Save changes");
  });
}

void account_role_form(Out& out, const ViewContext& ctx, const AccountUser& user) {
  const FormWith form = form_with_url(campfire::routes::account_user(user.user.id))
                            .model("user")
                            .data("controller", "form")
                            .method("patch");
  form_with(out, form, [&](Out& o) { views::accounts::users::role_form(o, ctx, user, form); });
}

std::string curl_text_line(std::string_view url) {
  return "curl -d 'Hello!' " + std::string(url);
}

std::string curl_upload_line(std::string_view url) {
  return "curl -F \"attachment=@/path/to/file\" " + std::string(url);
}

}  // namespace campfire::views::helpers
