// Route handler entry. Rust: crates/kit/src/adapter.rs.
#include "app/dispatch.hpp"

#include <exception>
#include <optional>

#include "core/log.hpp"

namespace campfire::app {

namespace {

// `ActionDispatch::SSL#redirect_to_https`: 301 for GET and HEAD, 308 otherwise.
Flow<net::Response> redirect_to_https(Rq& rq) {
  std::string host(rq.request.header("x-forwarded-host"));
  if (host.empty()) host = std::string(rq.request.header("host"));
  if (const std::size_t comma = host.rfind(','); comma != std::string::npos) host = host.substr(comma + 1);
  while (!host.empty() && host.front() == ' ') host.erase(host.begin());
  if (host.empty()) host = "localhost";
  if (const std::size_t colon = host.rfind(':'); colon != std::string::npos && colon + 1 < host.size() &&
                                                 host.find_first_not_of("0123456789", colon + 1) == std::string::npos) {
    host.resize(colon);
  }
  net::Response response = rq.ctx.response(rq.is_get() || rq.is_head() ? 301 : 308);
  response.add("content-type", "text/html");
  response.add_copy("location", "https://" + host + std::string(rq.request.target));
  return response;
}

}  // namespace

Task<net::Response> dispatch(net::Ctx& ctx, Action action) {
  Rq rq(ctx);
  // Response has no move assignment, so the result is built in place.
  std::optional<Flow<net::Response>> result;
  try {
    if (rq.app.proxy.force_ssl && !rq.info.ssl()) {
      result.emplace(redirect_to_https(rq));
    } else if (auto init = rq.init(); !init) {
      result.emplace(std::unexpected(std::move(init.error())));
    } else {
      result.emplace(co_await action(rq));
    }
  } catch (const std::exception& e) {
    log_error("action for {} {} threw: {}", rq.request.method_text, rq.request.path, e.what());
    result.emplace(fail_internal(e.what()));
  } catch (...) {
    log_error("action for {} {} threw", rq.request.method_text, rq.request.path);
    result.emplace(fail_internal("unknown exception"));
  }
  co_return rq.finish(std::move(*result));
}

}  // namespace campfire::app
