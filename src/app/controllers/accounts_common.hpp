// Pieces that the controllers of the account and user area share. Rails: ApplicationController, ActionController
// conditional get helpers. Rust: crates/kit/src/ctx.rs (expires_in), concerns.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "app/flow.hpp"
#include "app/rq.hpp"
#include "core/task.hpp"

namespace campfire::app::controllers {

// `expires_in seconds, public:, stale_while_revalidate:`: the cache control, and the `Date` header if the action did
// not set it (`response.date = Time.now unless response.date?`).
void expires_in(Rq& rq, std::uint64_t seconds, bool is_public, std::optional<std::uint64_t> stale_while_revalidate = {});

// The chain of ApplicationController, then `before_action :ensure_can_administer`.
[[nodiscard]] Task<Flow<void>> administrate(Rq& rq);

// `redirect_to edit_account_url` and the other redirects of the area: `redirect_to` with the URL of a path.
[[nodiscard]] Flow<net::Response> redirect_to_path(Rq& rq, std::string_view path, std::optional<std::string> notice = {});

// The user of `params[key]` as `User.find` casts it: nullopt where Rails raises `RecordNotFound`.
[[nodiscard]] std::optional<std::int64_t> id_param(Rq& rq, std::string_view key);

}  // namespace campfire::app::controllers
