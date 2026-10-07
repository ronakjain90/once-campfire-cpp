// UnfurlLinksController. Rails: app/controllers/unfurl_links_controller.rb. Rust: crates/campfire/src/controllers/
// unfurl_links.rs.
#include "app/concerns.hpp"
#include "app/dispatch.hpp"
#include "app/opengraph/opengraph.hpp"
#include "net/thread_pool.hpp"

namespace campfire::app::controllers {

namespace {

// The unfurls run on their own pool: one takes up to 10 seconds, and the pool of the bcrypt work must stay free.
// The pool is also the limit of 16 unfurls at once. The deadline starts when the request arrives.
net::ThreadPool& unfurl_pool() {
  static net::ThreadPool pool(opengraph::kMaxConcurrentUnfurls);
  return pool;
}

Task<Flow<net::Response>> unfurl_links_create(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  // `params.require(:url)`: a missing or blank `url` is a 400.
  const auto required = rq.params().require("url");
  if (!required) co_return fail_with(ErrorKind::ParameterMissing, required.error().message);
  // A hash or an array passes `require`, but `URI.parse` cannot take it (`InvalidURIError`, rescued): the metadata has
  // no title and is not valid.
  const auto text = (*required)->as_str();
  if (!text) co_return rq.head(204);
  const std::string url(*text);
  const auto deadline = unfurl::Clock::now() + opengraph::kUnfurlDeadline;
  auto result = co_await rq.ctx.offload(unfurl_pool(), [url, deadline]() -> Result<opengraph::Unfurl> {
    static const unfurl::Network network;
    return opengraph::unfurl(network, url, deadline);
  });
  if (!result) co_return fail_internal(result.error().message);
  if (!result->has_content) co_return rq.head(204);
  // `render json: opengraph`
  Out out(rq.ctx.resource());
  out.append_raw(result->json);
  co_return rq.render_as(200, "application/json; charset=utf-8", std::move(out));
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::unfurl_links {

Task<net::Response> create(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::unfurl_links_create);
}

}  // namespace campfire::routes::unfurl_links
