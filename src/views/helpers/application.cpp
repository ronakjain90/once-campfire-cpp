// Application helpers (Rust: crates/views/src/helpers/application.rs).
#include "views/helpers/application.hpp"

#include "routes/routes.hpp"
#include "views/helpers/assets.hpp"
#include "views/helpers/links.hpp"

namespace campfire::views::helpers {

void page_title_tag(Out& out, std::optional<std::string_view> page_title) {
  content_tag_text(out, "title", Attrs{}, page_title.value_or("Campfire"));
}

void current_user_meta_tags(Out& out, const ViewContext& ctx) {
  if (!ctx.current_user) return;
  legacy_tag(out, "meta", attrs().name("current-user-id").attr("content", ctx.current_user->id));
  legacy_tag(out, "meta", attrs().name("current-user-name").attr("content", ctx.current_user->name));
}

void script_aware_action_cable_meta_tag(Out& out, const ViewContext& ctx) {
  builder_tag(out, "meta", attrs().name("action-cable-url").attr("content", ctx.cable_url));
}

void custom_styles_tag(Out& out, const ViewContext& ctx) {
  if (!ctx.custom_styles) return;
  content_tag(out, "style", attrs().data("turbo_track", "reload"),
              [&](Out& o) { o.append(SafeHtml::trusted(*ctx.custom_styles)); });
}

std::string body_classes(const ViewContext& ctx, std::optional<std::string_view> body_class) {
  std::string out;
  const auto add = [&](std::string_view part) {
    if (!out.empty()) out += ' ';
    out += part;
  };
  if (body_class) add(*body_class);
  if (ctx.can_administer()) add("admin");
  if (ctx.account.has_logo) add("account-has-logo");
  return out;
}

void link_back_to(Out& out, const ViewContext& ctx, std::string_view destination) {
  link_to(out, destination, attrs().cls("btn"), [&](Out& o) {
    image_tag(o, ctx, "arrow-left.svg", attrs().aria_hidden().size(20));
    content_tag_text(o, "span", attrs().cls("for-screen-reader"), "Go Back");
  });
}

void link_back(Out& out, const ViewContext& ctx) {
  if (ctx.referrer && *ctx.referrer != ctx.request_url) {
    link_back_to(out, ctx, *ctx.referrer);
  } else {
    link_back_to(out, ctx, routes::root());
  }
}

void link_back_to_last_room_visited(Out& out, const ViewContext& ctx) {
  link_back_to(out, ctx, ctx.last_room_visited_id ? routes::room(*ctx.last_room_visited_id) : routes::root());
}

void version_badge(Out& out, const ViewContext& ctx) {
  content_tag_text(out, "span", attrs().cls("version-badge"), ctx.app_version);
}

}  // namespace campfire::views::helpers
