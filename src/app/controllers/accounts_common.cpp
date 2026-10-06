// Rust: crates/kit/src/ctx.rs (expires_in), crates/campfire/src/concerns.rs.
#include "app/controllers/accounts_common.hpp"

#include "app/concerns.hpp"
#include "compat/ruby.hpp"
#include "core/time_format.hpp"

namespace campfire::app::controllers {

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

std::optional<std::int64_t> id_param(Rq& rq, std::string_view key) {
  const auto text = rq.param_str(key);
  if (!text) return std::nullopt;
  return compat::integer_cast(*text);
}

}  // namespace campfire::app::controllers
