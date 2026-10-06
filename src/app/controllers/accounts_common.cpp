// Rust: crates/kit/src/ctx.rs (expires_in), crates/campfire/src/concerns.rs.
#include "app/controllers/accounts_common.hpp"

#include "app/concerns.hpp"
#include "compat/ruby.hpp"
#include "compat/signed_id.hpp"
#include "core/time_format.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::controllers {

compat::Timestamp to_compat(Timestamp t) {
  return compat::Timestamp(std::chrono::nanoseconds(t.seconds * 1'000'000'000LL + t.nanos));
}

std::string transfer_id(Rq& rq, std::int64_t user_id) {
  // `User::Transferable::TRANSFER_LINK_EXPIRY_DURATION`: 4 hours.
  constexpr std::int64_t kExpirySeconds = std::int64_t{4} * 60 * 60;
  return compat::signed_id::generate(rq.app.secrets, "User", user_id, "transfer",
                                     to_compat(rq.now().plus_seconds(kExpirySeconds)));
}

void expires_in(Rq& rq, std::uint64_t seconds, bool is_public, std::optional<std::uint64_t> stale_while_revalidate) {
  CacheControl& cc = rq.cache_control;
  cc.no_store = false;
  cc.max_age = seconds;
  cc.is_public = is_public;
  cc.stale_while_revalidate = stale_while_revalidate;
  if (rq.staged_header("date").empty()) rq.set_header("date", format_httpdate(rq.now()));
}

Task<Flow<void>> administrate(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  co_return concerns::ensure_can_administer(rq);
}

Flow<net::Response> redirect_to_path(Rq& rq, std::string_view path, std::optional<std::string> notice) {
  RedirectOptions options;
  options.notice = std::move(notice);
  return rq.redirect_to(rq.url_for(path), std::move(options));
}

net::Response render_tracked_page(Rq& rq, int status, db::DependencyScope& deps, const LayoutData& layout,
                                  const PageSpec& spec) {
  deps.facet("page", spec.name);
  deps.facet("title", spec.title);
  deps.facet("body_class", spec.body_class.value_or(""));
  deps.facet("body_class_given", static_cast<std::uint64_t>(spec.body_class.has_value()));
  add_layout_facets(rq, deps, layout);
  if (spec.facets) spec.facets(deps, layout);
  const auto render = [&](Out& out) {
    const views::ViewContext ctx = make_view_context(rq, layout);
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
  };
  return cached_page(rq, status, deps, render);
}

void add_link_back_facets(Rq& rq, db::DependencyScope& deps) {
  deps.facet("referer", rq.request.header("referer"));
  deps.facet("referer_given", static_cast<std::uint64_t>(rq.request.has_header("referer")));
  deps.facet("url", rq.info.url());
}

void add_platform_facet(Rq& rq, db::DependencyScope& deps) {
  deps.facet("user_agent", rq.user_agent());
}

std::optional<std::int64_t> id_param(Rq& rq, std::string_view key) {
  const auto text = rq.param_str(key);
  if (!text) return std::nullopt;
  return compat::integer_cast(*text);
}

}  // namespace campfire::app::controllers
