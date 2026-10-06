// Forgery protection (Rails: request_forgery_protection.rb; Rust: crates/kit/src/ctx.rs).
#include "req/forgery.hpp"

#include <cctype>

#include "compat/base64.hpp"
#include "compat/crypto.hpp"

namespace campfire::req {

std::optional<InvalidAuthenticityToken> verify_authenticity_token(const ForgeryInput& in) {
  if (in.exempt || in.method == "GET" || in.method == "HEAD") return std::nullopt;

  if (in.origin_check && in.origin) {
    if (*in.origin == "null") return InvalidAuthenticityToken{"The browser returned a 'null' origin"};
    if (*in.origin != in.base_url) {
      return InvalidAuthenticityToken{"HTTP Origin header (" + std::string(*in.origin) +
                                      ") didn't match request.base_url (" + std::string(in.base_url) + ")"};
    }
  }

  if (in.sec_fetch_site) {
    const std::string_view site = *in.sec_fetch_site;
    if (site == "same-origin" || site == "same-site") return std::nullopt;
    if (site == "cross-site") {
      return InvalidAuthenticityToken{"Sec-Fetch-Site header (cross-site) indicates a cross-site request"};
    }
    return InvalidAuthenticityToken{"Sec-Fetch-Site header is missing or invalid (Some(\"" + std::string(site) +
                                    "\"))"};
  }
  if (!in.ssl && !in.force_ssl) return std::nullopt;
  return InvalidAuthenticityToken{"Sec-Fetch-Site header is missing or invalid (None)"};
}

bool is_valid_authenticity_token(std::string_view session_token, std::string_view token, std::string_view path,
                                 std::string_view method) {
  constexpr std::size_t kLength = 32;  // AUTHENTICITY_TOKEN_LENGTH
  if (token.empty()) return false;
  const auto real = compat::base64::urlsafe_decode(session_token);
  const auto masked = compat::base64::urlsafe_decode(token);
  if (!real || !masked) return false;
  // fixed_length_secure_compare: a different length is not equal.
  const auto same = [](std::string_view a, std::string_view b) {
    return a.size() == b.size() && compat::crypto::constant_time_equal(a, b);
  };
  if (masked->size() == kLength) return same(*masked, *real);
  if (masked->size() != kLength * 2) return false;

  std::string unmasked(kLength, '\0');
  for (std::size_t i = 0; i < kLength; ++i) unmasked[i] = static_cast<char>((*masked)[i] ^ (*masked)[kLength + i]);

  const auto hmac = [&](std::string_view identifier) {
    return compat::crypto::hmac(compat::crypto::Digest::Sha256, *real, identifier);
  };
  if (same(unmasked, hmac("!real_csrf_token")) || same(unmasked, *real)) return true;
  std::string lower_method(method);
  for (char& c : lower_method) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  std::string_view action = path;
  if (action.ends_with('/')) action.remove_suffix(1);
  return same(unmasked, hmac(std::string(action) + "#" + lower_method));
}

bool is_cross_origin_javascript(bool marked, bool javascript_response, bool xhr) {
  return marked && javascript_response && !xhr;
}

}  // namespace campfire::req
