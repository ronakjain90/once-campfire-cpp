// Pieces that the controllers of the account and user area share. Rails: ApplicationController, ActionController
// conditional get helpers. Rust: crates/kit/src/ctx.rs (expires_in), concerns.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "app/flow.hpp"
#include "app/render_page.hpp"
#include "app/rq.hpp"
#include "compat/time.hpp"
#include "core/task.hpp"
#include "core/timestamp.hpp"

namespace campfire::app::controllers {

// The `Timestamp` of the clock as the one of `src/compat`.
[[nodiscard]] compat::Timestamp to_compat(Timestamp t);

// `user.transfer_id`: `signed_id(purpose: :transfer, expires_in: 4.hours)`. It depends on the clock.
[[nodiscard]] std::string transfer_id(Rq& rq, std::int64_t user_id);

// `expires_in seconds, public:, stale_while_revalidate:`: the cache control, and the `Date` header if the action did
// not set it (`response.date = Time.now unless response.date?`).
void expires_in(Rq& rq, std::uint64_t seconds, bool is_public, std::optional<std::uint64_t> stale_while_revalidate = {});

// The chain of ApplicationController, then `before_action :ensure_can_administer`.
[[nodiscard]] Task<Flow<void>> administrate(Rq& rq);

// `redirect_to edit_account_url` and the other redirects of the area: `redirect_to` with the URL of a path.
[[nodiscard]] Flow<net::Response> redirect_to_path(Rq& rq, std::string_view path, std::optional<std::string> notice = {});

// The page of `render_page`, for a controller that reads its data first: `deps = rq.track()` and `layout =
// load_layout(rq)` come before the reads, so that the reads are in the key of the page cache. This adds the facets
// of the layout and of `spec`, and sends the page from the cache (or renders it).
[[nodiscard]] net::Response render_tracked_page(Rq& rq, int status, db::DependencyScope& deps, const LayoutData& layout,
                                                const PageSpec& spec);
// The facets of `link_back`: the referrer and the URL of the request.
void add_link_back_facets(Rq& rq, db::DependencyScope& deps);
// The facet of a page that prints `platform`: the User-Agent.
void add_platform_facet(Rq& rq, db::DependencyScope& deps);

// The user of `params[key]` as `User.find` casts it: nullopt where Rails raises `RecordNotFound`.
[[nodiscard]] std::optional<std::int64_t> id_param(Rq& rq, std::string_view key);

}  // namespace campfire::app::controllers
