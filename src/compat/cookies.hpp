// cookies.signed[...] and cookies.encrypted[...] values (Rails: action_dispatch/middleware/cookies.rb;
// Rust: crates/rails_compat/src/cookies.rs), plus Rack's escaping of cookie values on the wire.
// Attributes (path, expires, HttpOnly, SameSite) belong to the HTTP layer.
//
// The functions work on the raw jar value. The value is dumped with ActiveSupport::JSON, then
// signed (HMAC-SHA1, key "signed cookie") or encrypted (aes-256-gcm), with the legacy envelope
// that carries pur "cookie.<name>" and exp. Reading tries purpose "cookie.<name>" first and then
// no purpose, so a value signed without metadata (before Rails 5.2) is accepted under any name.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "compat/json.hpp"
#include "compat/secrets.hpp"
#include "compat/time.hpp"

namespace campfire::compat::cookies {

// cookies.signed[name] = { value:, expires: expires_at }. cookies.signed.permanent is
// expires_at = permanent_expires_at(now).
std::string sign(const Secrets& secrets, std::string_view name, std::string_view value,
                 std::optional<Timestamp> expires_at);
// cookies.signed[name]: nullopt wherever Rails returns nil. A valid JSON value that is not a string is nullopt.
std::optional<std::string> verify_signed(const Secrets& secrets, std::string_view name, std::string_view raw,
                                         Timestamp now);
// cookies.signed[name] as whatever JSON value it holds.
std::optional<json::Value> verify_signed_value(const Secrets& secrets, std::string_view name, std::string_view raw,
                                               Timestamp now);

// cookies.encrypted[name] = { value:, expires: expires_at }.
std::string encrypt(const Secrets& secrets, std::string_view name, const json::Value& value,
                    std::optional<Timestamp> expires_at);
std::optional<json::Value> decrypt(const Secrets& secrets, std::string_view name, std::string_view raw, Timestamp now);

// Rack::Utils.escape (URI.encode_www_form_component): "*-._" and alphanumerics stay, a space
// is "+", the rest is %XX.
std::string escape(std::string_view raw);
// Rack's `unescape(value) rescue value`: "+" is a space and %XX is decoded. A malformed
// escape leaves the value unchanged.
std::string unescape(std::string_view wire);

}  // namespace campfire::compat::cookies
