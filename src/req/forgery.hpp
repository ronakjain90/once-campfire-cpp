// Forgery protection by Sec-Fetch-Site and Origin (Rails: ActionController::RequestForgeryProtection
// with `using: :header_only`; Rust: Ctx::verify_authenticity_token in crates/kit/src/ctx.rs).
// Pages carry no token. A token that an old page posts is ignored, as in the Rust port, and
// verify_authenticity_token does not read one. `is_valid_authenticity_token` is the Rails check
// of a masked token, for the golden vectors and for a caller that needs it.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace campfire::req {

struct ForgeryInput {
  std::string_view method;  // the effective method, after _method override: "GET", "POST"
  std::optional<std::string_view> origin;
  std::optional<std::string_view> sec_fetch_site;
  std::string_view base_url;  // request.base_url
  bool ssl = false;           // request.ssl?
  bool force_ssl = false;     // config.force_ssl
  bool origin_check = true;   // forgery_protection_origin_check
  // The hook for skip_forgery_protection: a request that a bot key authenticated.
  bool exempt = false;
};

// ActionController::InvalidAuthenticityToken: the controller answers 422.
struct InvalidAuthenticityToken {
  std::string message;
};

// The verify_authenticity_token before action. Returns nothing if the request may continue.
[[nodiscard]] std::optional<InvalidAuthenticityToken> verify_authenticity_token(const ForgeryInput& in);

// valid_authenticity_token?: `session_token` is session[:_csrf_token] (URL-safe Base64). The
// token is a masked token, a masked per-form token or an unmasked session token. `path` and
// `method` are request.path and request.request_method. Per-form tokens are on, as in Rails 8.
[[nodiscard]] bool is_valid_authenticity_token(std::string_view session_token, std::string_view token,
                                               std::string_view path, std::string_view method);

// verify_same_origin_request: a GET that renders JavaScript for a request that is not XHR is a
// cross-origin script embed. `marked` is true if the request was a GET at the before action.
[[nodiscard]] bool is_cross_origin_javascript(bool marked, bool javascript_response, bool xhr);

}  // namespace campfire::req
