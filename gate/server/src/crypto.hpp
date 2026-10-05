// Crypto, base64 and Rails signed-cookie support (port of rails_compat cookies + message_verifier).
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace gate {

std::string pbkdf2_sha256(std::string_view secret, std::string_view salt, int iterations, size_t len);
std::string hmac_sha1(std::string_view key, std::string_view data);    // raw bytes
std::string hmac_sha256(std::string_view key, std::string_view data);  // raw bytes
std::string sha256(std::string_view data);                             // raw bytes
std::string md5_hex(std::string_view data);
std::string hex(std::string_view raw);

std::string b64_strict_encode(std::string_view in);
std::string b64_url_encode_nopad(std::string_view in);
std::optional<std::string> b64_strict_decode(std::string_view in);
std::optional<std::string> b64_urlsafe_decode(std::string_view in);  // Ruby urlsafe_decode64

// Rack: URI.encode_www_form_component / unescape.
std::string www_escape(std::string_view in);
std::string www_unescape(std::string_view in);

// "YYYY-MM-DDTHH:MM:SS(.fff)Z" -> unix milliseconds.
std::optional<int64_t> parse_iso_ms(std::string_view s);
std::string iso_ms(int64_t unix_ms);

struct Secrets {
  std::string signed_cookie_key;  // generate_key("signed cookie", 64)
  std::string signed_id_key;      // generate_key("active_record/signed_id", 64)
  explicit Secrets(std::string_view secret_key_base);
};

// cookies.signed[name] for a legacy-envelope cookie; raw is the un-escaped jar value.
std::optional<std::string> verify_signed_cookie(const Secrets&, std::string_view name, std::string_view raw, int64_t now_ms);
// cookies.signed[name] = {value:, expires:} ; expires_ms < 0 means none.
std::string sign_cookie(const Secrets&, std::string_view name, std::string_view value, int64_t expires_ms);
// user.signed_id(purpose: "avatar") : User, id
std::string signed_id(const Secrets&, std::string_view model, int64_t id, std::string_view purpose);

}  // namespace gate
