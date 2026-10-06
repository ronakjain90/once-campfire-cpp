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

// `String#downcase` for the ASCII letters (the data-value of a user).
[[nodiscard]] std::string lowercase(std::string_view text);

}  // namespace campfire::views::helpers
