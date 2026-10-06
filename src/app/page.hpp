// Pages: the view context, the layouts and the page cache for a page. Rails: ActionView layouts, `cache`
// helpers, Rack::ETag. Rust: crates/campfire/src/controllers/presenters/view_context.rs (Layout).
//
// A page that can be cached reads its data in a tracked scope, adds its facets, and gives the render
// function to `cached_page`:
//
//   db::DependencyScope& deps = rq.track();
//   auto layout = load_layout(rq);            // reads the account: tracked
//   deps.facet("page", "sessions#new");       // every non SQL value that the page prints
//   return cached_page(rq, 200, deps, [&](Out& out) { render_in_layout(rq, *layout, parts, out); });
#pragma once

#include <functional>
#include <optional>
#include <string>

#include "app/flow.hpp"
#include "app/rq.hpp"
#include "models/user.hpp"
#include "views/context.hpp"
#include "views/layout.hpp"

namespace campfire::app {

// What the layout needs besides the request: the current user, the account, the custom styles.
struct LayoutData {
  std::optional<views::CurrentUser> current_user;
  views::AccountSummary account;
  std::optional<std::string> custom_styles;
  std::optional<std::int64_t> last_room_visited_id;  // `last_room_visited` (rooms: a later task fills it)
};

// `fresh_user_avatar_path(user)`: the signed avatar token and the `v` cache buster.
[[nodiscard]] std::string user_avatar_path(const Rq& rq, const models::User& user);

// `Current.account`, `Current.user`. With no account yet (first run) the account summary is blank.
[[nodiscard]] Flow<LayoutData> load_layout(Rq& rq);

// The `ViewContext` of this request. It reads the flash, which marks it as used.
[[nodiscard]] views::ViewContext make_view_context(Rq& rq, const LayoutData& data);

// Renders `parts` in the application layout, or in turbo-rails' frame layout for a Turbo-Frame request.
void render_in_layout(Rq& rq, const LayoutData& data, const views::LayoutParts& parts, Out& out);

// The facets that every page in the layout prints and that are not SQL: the page name, the base URL, the frame
// flag, the current user (the layout shows the avatar) and the flash. It reads the flash.
void add_page_facets(Rq& rq, db::DependencyScope& deps, std::string_view page);

// A page in the layout, with the `Link` preload header of `stylesheet_link_tag`. Not cached.
[[nodiscard]] net::Response layout_response(Rq& rq, int status, Out&& body);

// The page cache path of architecture 6.1. `render` writes the page to `out`. The key is the hash of
// `deps`. A hit sends the cached entry. On 1 hit in 16 in the sanitizer builds, the page is rendered
// again and compared (a difference is fatal). A scope marked uncacheable renders each time.
// `preload_link` adds the `Link` header of `stylesheet_link_tag`: false for a response without a layout.
[[nodiscard]] net::Response cached_page(Rq& rq, int status, db::DependencyScope& deps,
                                        const std::function<void(Out&)>& render, bool preload_link = true);

}  // namespace campfire::app
