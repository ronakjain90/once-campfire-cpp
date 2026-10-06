// The pages of the room forms in the application layout. Rust: crates/views/src/rooms.rs.
#include "views/rooms/pages.hpp"

#include "views/templates.gen.hpp"

namespace campfire::views::rooms {

namespace {

// `rooms/layouts/_new` and `rooms/layouts/_edit`: the nav is the link back.
void fill_nav(LayoutParts& parts, const ViewContext& ctx) {
  parts.nav = [&ctx](Out& out) { layouts::nav(out, ctx); };
}

std::string edit_title(const FormRoom& room) {
  return "Edit settings for " + std::string(room.display_name());
}

}  // namespace

void opens_new(LayoutParts& parts, std::string& title, const ViewContext& ctx, const OpenFormView& form) {
  title = "New chat room";
  parts.page_title = title;
  fill_nav(parts, ctx);
  parts.content = [&](Out& out) { opens::new_(out, ctx, form); };
}

void opens_edit(LayoutParts& parts, std::string& title, const ViewContext& ctx, const OpenFormView& form) {
  title = edit_title(form.room);
  parts.page_title = title;
  fill_nav(parts, ctx);
  parts.content = [&](Out& out) { opens::edit(out, ctx, form); };
}

void closeds_new(LayoutParts& parts, std::string& title, const ViewContext& ctx, const ClosedFormView& form) {
  title = "New chat room";
  parts.page_title = title;
  fill_nav(parts, ctx);
  parts.content = [&](Out& out) { closeds::new_(out, ctx, form); };
}

void closeds_edit(LayoutParts& parts, std::string& title, const ViewContext& ctx, const ClosedFormView& form) {
  title = edit_title(form.room);
  parts.page_title = title;
  fill_nav(parts, ctx);
  parts.content = [&](Out& out) { closeds::edit(out, ctx, form); };
}

void directs_new(LayoutParts& parts, const ViewContext& ctx) {
  parts.content = [&](Out& out) { directs::new_(out, ctx); };
}

void directs_edit(LayoutParts& parts, std::string& title, const ViewContext& ctx, const DirectEditView& edit) {
  title = "Edit settings for " + edit.display_name;
  parts.page_title = title;
  fill_nav(parts, ctx);
  parts.content = [&](Out& out) { directs::edit(out, ctx, edit); };
}

}  // namespace campfire::views::rooms
