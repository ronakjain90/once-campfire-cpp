// GET /up. Rails: Rails::HealthController (rails/health#show); Rust: crates/campfire health route.
#include "app/data.hpp"
#include "app/rails.hpp"
#include "net/ctx.hpp"

namespace campfire::routes::health {

campfire::Task<net::Response> show(net::Ctx& ctx) {
  if (app::refuses_every_encoding(ctx.request())) co_return app::not_acceptable(ctx);
  net::Response response = ctx.response(200);
  const bool gzip = ctx.request().method != net::Method::Head && app::wants_gzip(ctx.request());
  response.add("content-type", "text/html; charset=utf-8");
  response.add("x-frame-options", "SAMEORIGIN");
  response.add("x-xss-protection", "0");
  response.add("x-content-type-options", "nosniff");
  response.add("x-permitted-cross-domain-policies", "none");
  response.add("referrer-policy", "strict-origin-when-cross-origin");
  response.add("etag", "W/\"7e6c9877b2dec7dfadc43e742246d94d\"");
  response.add("cache-control", "max-age=0, private, must-revalidate");
  if (gzip) {
    response.add("content-encoding", "gzip");
    response.chunked = true;
    response.body_view(app::data::f_up_html_gz);
  } else {
    response.add("content-length", "73");
    response.body_view(app::data::f_up_html);
  }
  app::add_rails_tail(ctx, response);
  app::add_hsts(ctx.request(), response);
  response.add("vary", "Accept-Encoding");  // Rack::Deflater, after the tail
  co_return response;
}

}  // namespace campfire::routes::health
