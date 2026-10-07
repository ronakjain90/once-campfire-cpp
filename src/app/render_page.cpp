// A page in the application layout. Rust: crates/campfire/src/controllers/presenters/page.rs (framed_page!).
#include "app/render_page.hpp"

#include "views/templates.gen.hpp"

namespace campfire::app {

void add_layout_facets(Rq& rq, db::DependencyScope& deps, const LayoutData& data) {
  deps.facet("base_url", rq.info.base_url());
  deps.facet("frame", static_cast<std::uint64_t>(rq.is_turbo_frame_request()));
  const auto notice = rq.flash().notice();
  const auto alert = rq.flash().alert();
  deps.facet("notice", notice.value_or(""));
  deps.facet("notice_given", static_cast<std::uint64_t>(notice.has_value()));
  deps.facet("alert", alert.value_or(""));
  deps.facet("alert_given", static_cast<std::uint64_t>(alert.has_value()));
  // The current user comes from the session cache, not from SQL of this request.
  deps.facet("user_given", static_cast<std::uint64_t>(data.current_user.has_value()));
  if (data.current_user) {
    deps.facet("user_id", static_cast<std::uint64_t>(data.current_user->id));
    deps.facet("user_name", data.current_user->name);
    deps.facet("user_admin", static_cast<std::uint64_t>(data.current_user->administrator));
    deps.facet("user_bot", static_cast<std::uint64_t>(data.current_user->bot));
    deps.facet("user_avatar", data.current_user->avatar_url);
  }
}

Flow<net::Response> render_page(Rq& rq, int status, const PageSpec& spec) {
  db::DependencyScope& deps = rq.track();
  auto layout = load_layout(rq);
  if (!layout) return std::unexpected(std::move(layout.error()));
  return render_page(rq, status, spec, deps, *layout);
}

Flow<net::Response> render_page(Rq& rq, int status, const PageSpec& spec, db::DependencyScope& deps,
                                const LayoutData& layout_data) {
  const LayoutData* layout = &layout_data;
  deps.facet("page", spec.name);
  deps.facet("title", spec.title);
  deps.facet("body_class", spec.body_class.value_or(""));
  deps.facet("body_class_given", static_cast<std::uint64_t>(spec.body_class.has_value()));
  add_layout_facets(rq, deps, *layout);
  if (spec.facets) spec.facets(deps, *layout);
  const LayoutData& data = *layout;
  const auto render = [&](Out& out) -> Flow<void> {
    if (spec.prepare) {
      if (auto prepared = spec.prepare(); !prepared) return prepared;
    }
    const views::ViewContext ctx = make_view_context(rq, data);
    const auto bind = [&](const PagePart& part) -> views::Region {
      if (!part) return {};
      return [&part, &ctx](Out& o) { part(o, ctx); };
    };
    views::LayoutParts parts;
    if (!spec.title.empty()) parts.page_title = spec.title;
    parts.body_class = spec.body_class;
    parts.head = bind(spec.head);
    parts.nav = bind(spec.nav);
    parts.content = bind(spec.content);
    parts.footer = bind(spec.footer);
    parts.sidebar = bind(spec.sidebar);
    if (rq.is_turbo_frame_request()) {
      views::layouts::turbo_rails::frame(out, parts);
    } else {
      views::layouts::application(out, ctx, parts);
    }
    return {};
  };
  return cached_page_checked(rq, status, deps, render, true, spec.parts_etag);
}

}  // namespace campfire::app
