// The pages of the room forms in the application layout: the title and the regions (head, nav, content).
// Rust: crates/views/src/rooms.rs (the `Page` impls and the blocks of the templates).
#pragma once

#include <string>

#include "views/context.hpp"
#include "views/layout.hpp"
#include "views/rooms/forms.hpp"

namespace campfire::views::rooms {

// Each function fills `parts`. `title` holds the text of the page title: it must live as long as `parts`.
// `parts` also keeps references to `ctx` and to the view model.
void opens_new(LayoutParts& parts, std::string& title, const ViewContext& ctx, const OpenFormView& form);
void opens_edit(LayoutParts& parts, std::string& title, const ViewContext& ctx, const OpenFormView& form);
void closeds_new(LayoutParts& parts, std::string& title, const ViewContext& ctx, const ClosedFormView& form);
void closeds_edit(LayoutParts& parts, std::string& title, const ViewContext& ctx, const ClosedFormView& form);

void directs_new(LayoutParts& parts, const ViewContext& ctx);
void directs_edit(LayoutParts& parts, std::string& title, const ViewContext& ctx, const DirectEditView& edit);

}  // namespace campfire::views::rooms
