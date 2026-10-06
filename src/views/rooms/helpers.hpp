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

// `String#downcase` for the ASCII letters (the data-value of a user).
[[nodiscard]] std::string lowercase(std::string_view text);

}  // namespace campfire::views::helpers
