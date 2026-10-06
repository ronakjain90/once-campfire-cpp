// The ACME protocol (RFC 8555) for one account. Used by the thread of `CertManager` only.
#pragma once

#include <string>
#include <string_view>

#include "net/front/acme_http.hpp"
#include "net/front/tls_keys.hpp"

namespace campfire::net::front {

class AcmeAccount {
 public:
  AcmeAccount(std::string directory_url, std::string ca_file, int timeout_seconds)
      : directory_url_(std::move(directory_url)), ca_file_(std::move(ca_file)), timeout_(timeout_seconds) {}

  // Sets the account key. `existing` is true if the key came from the cache file (the account
  // exists already).
  void set_key(PKeyPtr key, bool existing) {
    key_ = std::move(key);
    existing_ = existing;
  }
  [[nodiscard]] EVP_PKEY* key() const noexcept { return key_.get(); }
  [[nodiscard]] bool has_key() const noexcept { return static_cast<bool>(key_); }

  // Loads the directory and registers (or finds) the account. External account binding when both
  // `eab_kid` and the decoded `eab_hmac` are set.
  [[nodiscard]] Status register_account(const std::string& eab_kid, const std::string& eab_hmac);

  // The JWK thumbprint based key authorization of a token: "<token>.<thumbprint>".
  [[nodiscard]] std::string key_authorization(std::string_view token) const;

  // POST-as-GET (empty `payload` with `as_get`) or a POST with a JSON payload, signed with the account.
  [[nodiscard]] Result<HttpResult> post(const std::string& url, std::string_view payload, bool as_get);
  [[nodiscard]] Result<Json> post_json(const std::string& url, std::string_view payload, bool as_get,
                                       std::string* location = nullptr);

  [[nodiscard]] const std::string& new_order_url() const noexcept { return new_order_; }

 private:
  [[nodiscard]] Result<HttpResult> signed_request(const std::string& url, std::string_view payload, bool as_get,
                                                  bool use_jwk);
  [[nodiscard]] Result<std::string> sign(std::string_view protected_json, std::string_view payload_b64) const;
  [[nodiscard]] Result<std::string> jwk_json() const;
  [[nodiscard]] Status fetch_nonce();

  std::string directory_url_;
  std::string ca_file_;
  int timeout_;
  PKeyPtr key_;
  bool existing_ = false;
  std::string new_nonce_;
  std::string new_account_;
  std::string new_order_;
  std::string kid_;
  std::string nonce_;
};

}  // namespace campfire::net::front
