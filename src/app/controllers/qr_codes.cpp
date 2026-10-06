// QrCodeController. Rails: app/controllers/qr_code_controller.rb. Rust: crates/campfire/src/controllers/qr_code.rs.
#include "app/concerns.hpp"
#include "app/dispatch.hpp"
#include "app/rqrcode.hpp"
#include "compat/base64.hpp"

namespace campfire::app::controllers {

namespace {

// `allow_unauthenticated_access`
Task<Flow<net::Response>> qr_codes_show(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{}.allow_unauthenticated_access());
  if (!before) co_return std::unexpected(std::move(before.error()));
  // `Base64.urlsafe_decode64(params[:id])` raises ArgumentError (a 500) on malformed input.
  const auto url = compat::base64::urlsafe_decode(rq.param_str("id").value_or(""));
  if (!url) co_return fail_internal("invalid base64");
  // Data that is too long is the client's doing: rqrcode raises (a 500 in Rails), the Rust port answers 422.
  const auto svg = rqrcode::svg(*url);
  if (!svg) co_return fail_status(422, "Data length exceed maximum capacity of version 40");
  // `expires_in 1.year, public: true`
  rq.cache_control.max_age = 31'556'952;
  rq.cache_control.is_public = true;
  Out out(rq.ctx.resource());
  out.append_raw(*svg);
  co_return rq.render_as(200, "image/svg+xml; charset=utf-8", std::move(out));
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::qr_codes {

Task<net::Response> show(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::qr_codes_show);
}

}  // namespace campfire::routes::qr_codes
