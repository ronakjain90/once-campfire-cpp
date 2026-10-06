// The helpers of the account and user pages: the invite buttons, the avatar color, the profile submit button.
// Rails: app/helpers/qr_code_helper.rb, clipboard_helper.rb, users/avatars_helper.rb, users/profiles_helper.rb.
// Rust: crates/views/src/helpers/application.rs, users.rs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "views/context.hpp"
#include "views/helpers/assets.hpp"
#include "views/helpers/links.hpp"
#include "views/helpers/tag.hpp"

namespace campfire::views::helpers {

// `avatar_background_color(user)`: `AVATAR_COLORS[Zlib.crc32(user.to_param) % 18]`.
[[nodiscard]] std::string_view avatar_background_color(std::int64_t user_id);

// `link_to_zoom_qr_code(url) do ... end`: the link to `/qr_code/<urlsafe base64 of url>`.
template <BodyFn Body>
void link_to_zoom_qr_code(Out& out, std::string_view url, Body&& body);
// `button_to_copy_to_clipboard(url) do ... end`.
template <BodyFn Body>
void button_to_copy_to_clipboard(Out& out, std::string_view url, Body&& body) {
  content_tag(out, "button",
              attrs()
                  .cls("btn")
                  .data("controller", "copy-to-clipboard")
                  .data("action", "copy-to-clipboard#copy")
                  .data("copy_to_clipboard_success_class", "btn--success")
                  .data("copy_to_clipboard_content_value", url),
              std::forward<Body>(body));
}
// `web_share_session_button(url, title, text) do ... end`.
template <BodyFn Body>
void web_share_session_button(Out& out, std::string_view url, std::string_view title, std::string_view text,
                              Body&& body) {
  content_tag(out, "button",
              attrs()
                  .cls("btn")
                  .hidden()
                  .data("controller", "web-share")
                  .data("action", "web-share#share")
                  .data("web_share_url_value", url)
                  .data("web_share_text_value", text)
                  .data("web_share_title_value", title),
              std::forward<Body>(body));
}

// `Base64.urlsafe_encode64(url)` for the QR code path: `qr_code_path(id)`.
[[nodiscard]] std::string qr_code_path_for(std::string_view url);

template <BodyFn Body>
void link_to_zoom_qr_code(Out& out, std::string_view url, Body&& body) {
  const std::string path = qr_code_path_for(url);
  link_to(out, path,
          attrs()
              .cls("btn")
              .data("lightbox_target", "image")
              .data("action", "lightbox#open")
              .data("lightbox_url_value", path),
          std::forward<Body>(body));
}

// `profile_form_submit_button`.
void profile_form_submit_button(Out& out, const ViewContext& ctx);

// The curl lines of `accounts/bots/_bot`.
[[nodiscard]] std::string curl_text_line(std::string_view url);
[[nodiscard]] std::string curl_upload_line(std::string_view url);

}  // namespace campfire::views::helpers
