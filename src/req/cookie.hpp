// The cookie jar (Rails: action_dispatch/middleware/cookies.rb, Rack::Utils cookie functions;
// Rust: crates/kit/src/cookies.rs). One jar for each request.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compat/json.hpp"
#include "compat/secrets.hpp"
#include "core/clock.hpp"
#include "core/error.hpp"
#include "core/timestamp.hpp"

namespace campfire::req {

inline constexpr std::size_t kMaxCookieSize = 4096;  // ActionDispatch::Cookies::MAX_COOKIE_SIZE

enum class SameSite : std::uint8_t { None, Lax, Strict };

// A cookie to set, with the Rails option names. Rails defaults: path "/" and SameSite=Lax.
struct Cookie {
  std::string value;
  std::string path = "/";
  std::optional<std::string> domain;
  std::optional<Timestamp> expires;
  bool permanent = false;  // expires 20 calendar years from now
  bool secure = false;
  bool httponly = false;
  std::optional<SameSite> same_site = SameSite::Lax;
  bool partitioned = false;
};

// Options of cookies.delete(name, options).
struct DeleteOptions {
  std::string path = "/";
  std::optional<std::string> domain;
  std::optional<SameSite> same_site = SameSite::Lax;
};

// Rack::Utils.parse_cookies_header: the first value of a name wins. A value that does not
// unescape stays as it is. Pairs are in the order of the header.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> parse_cookie_header(std::string_view header);

// Rack::Utils.set_cookie_header and delete_set_cookie_header.
[[nodiscard]] std::string set_cookie_header(std::string_view name, const Cookie& cookie);
[[nodiscard]] std::string delete_cookie_header(std::string_view name, const DeleteOptions& options);

class CookieJar {
 public:
  // `secrets` and `clock` must outlive the jar. A request can send more than one Cookie header.
  CookieJar(const std::vector<std::string_view>& headers, const compat::Secrets& secrets, const Clock& clock);

  // cookies[name]
  [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const;
  [[nodiscard]] bool contains(std::string_view name) const { return get(name).has_value(); }
  // cookies.signed[name]
  [[nodiscard]] std::optional<std::string> signed_value(std::string_view name) const;
  // cookies.encrypted[name]
  [[nodiscard]] std::optional<compat::json::Value> encrypted_value(std::string_view name) const;

  // cookies[name] = ... Writes a header only if the value changes or the cookie has an expiry.
  void set(std::string_view name, Cookie cookie);
  // cookies.signed[name] = ... Fails with CookieOverflow if name and value are over 4096 bytes.
  [[nodiscard]] Status set_signed(std::string_view name, Cookie cookie);
  // cookies.encrypted[name] = value.
  [[nodiscard]] Status set_encrypted(std::string_view name, const compat::json::Value& value, Cookie cookie);
  // cookies.delete(name): does nothing if the cookie is not there.
  void remove(std::string_view name, const DeleteOptions& options = {});
  [[nodiscard]] bool is_deleted(std::string_view name) const;

  // The Set-Cookie values, in order: sets, then deletes. A secure cookie needs SSL (or an
  // ".onion" host).
  [[nodiscard]] std::vector<std::string> set_cookie_headers(bool ssl, std::string_view host) const;

 private:
  void resolve_expiry(Cookie& cookie) const;
  void write_value(std::string_view name, Cookie cookie);

  std::vector<std::pair<std::string, std::string>> cookies_;
  std::vector<std::pair<std::string, Cookie>> set_cookies_;
  std::vector<std::pair<std::string, DeleteOptions>> delete_cookies_;
  const compat::Secrets* secrets_;
  const Clock* clock_;
};

}  // namespace campfire::req
