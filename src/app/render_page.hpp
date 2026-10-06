// A page of an action: the layout, the facets of the page cache and the cached response. Rails: the controller layout
// (`layout -> { "turbo_rails/frame" if turbo_frame_request? }`), `content_for`. Rust: controllers/presenters/page.rs
// (framed_page!). Built on `cached_page` of page.hpp.
#pragma once

#include <functional>
#include <optional>
#include <string_view>

#include "app/flow.hpp"
#include "app/page.hpp"
#include "app/rq.hpp"
#include "views/context.hpp"

namespace campfire::app {

// A part of a page. It writes with its own indent and its own newline, as the layout regions do.
using PagePart = std::function<void(Out&, const views::ViewContext&)>;

struct PageSpec {
  // The name of the page in the cache key: a page that prints other data than its name and the facets must add them.
  std::string_view name;
  std::string_view title;  // `@page_title`; empty: the layout prints "Campfire"
  std::optional<std::string_view> body_class;
  PagePart head;
  PagePart nav;
  PagePart content;
  PagePart footer;
  PagePart sidebar;
  // Reads the data that only a render needs (it runs on a page cache miss, before the parts). A failure is the result.
  std::function<Flow<void>()> prepare;
  // Called with the scope and the layout data, after the facets of the layout. Add the facets of the page here.
  std::function<void(db::DependencyScope&, const LayoutData&)> facets;
};

// Reads the layout data (tracked), adds the facets, and sends the page from the page cache (or renders and stores it).
// `status` is the status of the response.
[[nodiscard]] Flow<net::Response> render_page(Rq& rq, int status, const PageSpec& spec);

// The same, for a handler that has read its data in the scope `deps` already (`rq.track()` was called before the reads).
// `layout` is the result of `load_layout`, which ran in the same scope.
[[nodiscard]] Flow<net::Response> render_page(Rq& rq, int status, const PageSpec& spec, db::DependencyScope& deps,
                                              const LayoutData& layout);

// The facets of everything that the layouts print and that is not from tracked SQL: the host, the Turbo-Frame flag,
// the flash and the current user.
void add_layout_facets(Rq& rq, db::DependencyScope& deps, const LayoutData& data);

}  // namespace campfire::app
