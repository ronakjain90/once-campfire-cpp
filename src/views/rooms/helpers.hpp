// Block helpers and small helpers of the room templates. Rust: crates/views/src/rooms.rs (filters::room_form),
// crates/views/src/helpers/rooms.rs.
#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "core/out.hpp"
#include "views/context.hpp"
#include "views/rooms/forms.hpp"

namespace campfire::views::helpers {

// `render layout: "rooms/layouts/form", locals: { room: } do ... end`: the body goes to the layout as text.
void room_form(Out& out, const ViewContext& ctx, const FormRoom& room, bool can_administer, RoomKind kind,
               const std::function<void(Out&)>& body);

// `humanize_involvement`, `next_involvement_for(room, involvement:)`.
[[nodiscard]] std::string_view humanize_involvement(std::string_view involvement);
[[nodiscard]] std::string_view next_involvement(bool direct, std::string_view involvement);
// `button_to_change_involvement(room, involvement)`.
void button_to_change_involvement(Out& out, const ViewContext& ctx, const InvolvementView& view);

// The options of the join link buttons of the invitation: `link_to_zoom_qr_code(url)`,
// `button_to_copy_to_clipboard(url)` and `web_share_session_button(url, title, text)`. Rails:
// app/helpers/qr_code_helper.rb, application_helper.rb and users/profiles_helper.rb.
[[nodiscard]] std::string zoom_qr_path(std::string_view url);
[[nodiscard]] Attrs zoom_qr_options(std::string_view url);
[[nodiscard]] Attrs copy_to_clipboard_options(std::string_view url);
[[nodiscard]] Attrs web_share_session_options(std::string_view url, std::string_view title, std::string_view text);

// `String#downcase` for the ASCII letters (the data-value of a user).
[[nodiscard]] std::string lowercase(std::string_view text);

}  // namespace campfire::views::helpers
