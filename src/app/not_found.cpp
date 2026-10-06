// Unknown route. Rails: ActionDispatch::PublicExceptions (public/404.html); Rust: crates/kit/src/exceptions.rs.
#include "app/not_found.hpp"

#include "app/data.hpp"
#include "app/rails.hpp"

namespace campfire::app {

namespace {

// The format of the request: ".json" suffix, or JSON as the first media type of Accept.
bool wants_json(const net::Request& request) noexcept {
  if (request.path.ends_with(".json")) return true;
  const std::string_view accept = request.header("accept");
  return accept.starts_with("application/json");
}

}  // namespace

campfire::Task<net::Response> not_found(net::Ctx& ctx) {
  const net::Request& request = ctx.request();
  net::Response response = ctx.response(404);
  const bool head = request.method == net::Method::Head;
  // The "content-length: 0" of a HEAD answer keeps Rack::Deflater from touching it.
  if (!head && refuses_every_encoding(request)) co_return not_acceptable(ctx);
  if (wants_json(request)) {
    constexpr std::string_view kBody = R"({"status":404,"error":"Not Found"})";
    response.add("content-type", "application/json; charset=UTF-8");
    response.add("content-length", head ? "0" : "34");
    if (!head) response.body_view(kBody);
  } else if (head) {
    response.add("content-type", "text/html; charset=UTF-8");
    response.add("content-length", "0");
  } else if (wants_gzip(request)) {
    response.add("content-type", "text/html; charset=UTF-8");
    response.add("content-encoding", "gzip");
    response.framed = true;  // the Rust port sends this body in two chunks
    response.body_view(data::f_404_html_gz_chunked);
  } else {
    response.add("content-type", "text/html; charset=UTF-8");
    response.add("content-length", "4237");
    response.body_view(data::f_404_html);
  }
  add_rails_tail(ctx, response);
  // Rack::Deflater adds Vary to a body that it may compress (not to the empty body of HEAD).
  if (!head) response.add("vary", "Accept-Encoding");
  co_return response;
}

}  // namespace campfire::app
