// The ACME protocol (RFC 8555) for one account.
#include "net/front/acme_account.hpp"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include "compat/base64.hpp"

namespace campfire::net::front {

namespace {

std::string b64(std::string_view data) { return compat::base64::urlsafe_encode_unpadded(data); }

std::string json_escape(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

}  // namespace

Result<std::string> AcmeAccount::jwk_json() const {
  BIGNUM* x = nullptr;
  BIGNUM* y = nullptr;
  if (EVP_PKEY_get_bn_param(key_.get(), OSSL_PKEY_PARAM_EC_PUB_X, &x) != 1 ||
      EVP_PKEY_get_bn_param(key_.get(), OSSL_PKEY_PARAM_EC_PUB_Y, &y) != 1) {
    BN_free(x);
    BN_free(y);
    return fail(Errc::Internal, "cannot read the public key of the account");
  }
  unsigned char xb[32];
  unsigned char yb[32];
  BN_bn2binpad(x, xb, 32);
  BN_bn2binpad(y, yb, 32);
  BN_free(x);
  BN_free(y);
  // The members are in the order of the RFC 7638 thumbprint: crv, kty, x, y.
  return std::string(R"({"crv":"P-256","kty":"EC","x":")") + b64({reinterpret_cast<char*>(xb), 32}) + R"(","y":")" +
         b64({reinterpret_cast<char*>(yb), 32}) + "\"}";
}

std::string AcmeAccount::key_authorization(std::string_view token) const {
  auto jwk = jwk_json();
  if (!jwk) return {};
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(jwk->data()), jwk->size(), digest);
  return std::string(token) + "." + b64({reinterpret_cast<char*>(digest), sizeof digest});
}

Result<std::string> AcmeAccount::sign(std::string_view protected_json, std::string_view payload_b64) const {
  const std::string input = b64(protected_json) + "." + std::string(payload_b64);
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key_.get()) != 1) return fail(Errc::Internal, "sign init failed");
  std::size_t size = 0;
  if (EVP_DigestSign(ctx.get(), nullptr, &size, reinterpret_cast<const unsigned char*>(input.data()), input.size()) != 1) {
    return fail(Errc::Internal, "sign failed");
  }
  std::string der(size, '\0');
  if (EVP_DigestSign(ctx.get(), reinterpret_cast<unsigned char*>(der.data()), &size,
                     reinterpret_cast<const unsigned char*>(input.data()), input.size()) != 1) {
    return fail(Errc::Internal, "sign failed");
  }
  const auto* p = reinterpret_cast<const unsigned char*>(der.data());
  ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(size));  // NOLINT(google-runtime-int)
  if (sig == nullptr) return fail(Errc::Internal, "bad signature");
  unsigned char raw[64];
  BN_bn2binpad(ECDSA_SIG_get0_r(sig), raw, 32);
  BN_bn2binpad(ECDSA_SIG_get0_s(sig), raw + 32, 32);
  ECDSA_SIG_free(sig);
  return input + "." + b64({reinterpret_cast<char*>(raw), sizeof raw});
}

Status AcmeAccount::fetch_nonce() {
  auto result = http_request("HEAD", new_nonce_, {}, {}, ca_file_, timeout_);
  if (!result) return std::unexpected(result.error());
  nonce_ = result->header("replay-nonce");
  if (nonce_.empty()) return fail(Errc::Io, "the ACME server gave no nonce");
  return {};
}

Result<HttpResult> AcmeAccount::signed_request(const std::string& url, std::string_view payload, bool as_get, bool use_jwk) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (nonce_.empty()) {
      if (auto status = fetch_nonce(); !status) return std::unexpected(status.error());
    }
    std::string protected_json = R"({"alg":"ES256","nonce":")" + nonce_ + R"(","url":")" + json_escape(url) + "\",";
    if (use_jwk) {
      auto jwk = jwk_json();
      if (!jwk) return std::unexpected(jwk.error());
      protected_json += "\"jwk\":" + *jwk + "}";
    } else {
      protected_json += "\"kid\":\"" + json_escape(kid_) + "\"}";
    }
    nonce_.clear();
    auto jws = sign(protected_json, as_get ? std::string() : b64(payload));
    if (!jws) return std::unexpected(jws.error());
    const std::size_t dot2 = jws->rfind('.');
    const std::size_t dot1 = jws->find('.');
    const std::string body = R"({"protected":")" + jws->substr(0, dot1) + R"(","payload":")" +
                             jws->substr(dot1 + 1, dot2 - dot1 - 1) + R"(","signature":")" + jws->substr(dot2 + 1) + "\"}";
    auto result = http_request("POST", url, {{"content-type", "application/jose+json"}}, body, ca_file_, timeout_);
    if (!result) return result;
    if (const std::string next = result->header("replay-nonce"); !next.empty()) nonce_ = next;
    if (result->status == 400 && result->body.find("badNonce") != std::string::npos && attempt == 0) continue;
    return result;
  }
  return fail(Errc::Io, "the ACME server refused the nonce");
}

Result<HttpResult> AcmeAccount::post(const std::string& url, std::string_view payload, bool as_get) {
  return signed_request(url, payload, as_get, false);
}

Result<Json> AcmeAccount::post_json(const std::string& url, std::string_view payload, bool as_get, std::string* location) {
  auto result = post(url, payload, as_get);
  if (!result) return std::unexpected(result.error());
  if (result->status < 200 || result->status >= 300) {
    return fail(Errc::Io, "ACME " + url + " answered " + std::to_string(result->status) + ": " + result->body);
  }
  if (location != nullptr) *location = result->header("location");
  return Json::parse(result->body);
}

Status AcmeAccount::register_account(const std::string& eab_kid, const std::string& eab_hmac) {
  auto directory = http_request("GET", directory_url_, {}, {}, ca_file_, timeout_);
  if (!directory) return std::unexpected(directory.error());
  auto json = Json::parse(directory->body);
  if (!json) return std::unexpected(json.error());
  new_nonce_ = std::string(json->str("newNonce"));
  new_account_ = std::string(json->str("newAccount"));
  new_order_ = std::string(json->str("newOrder"));
  if (new_nonce_.empty() || new_account_.empty() || new_order_.empty()) return fail(Errc::Parse, "the ACME directory is not complete");

  const auto register_with = [&](std::string_view payload, std::string& location) -> Status {
    auto result = signed_request(new_account_, payload, false, true);
    if (!result) return std::unexpected(result.error());
    if (result->status < 200 || result->status >= 300) {
      return fail(Errc::Io, "ACME newAccount answered " + std::to_string(result->status) + ": " + result->body);
    }
    location = result->header("location");
    return {};
  };
  std::string location;
  if (existing_) {
    // The key came from the cache file: find its account. If the server has none, make one.
    if (register_with(R"({"onlyReturnExisting":true})", location)) {
      kid_ = location;
      return {};
    }
  }
  std::string payload = R"({"termsOfServiceAgreed":true,"contact":[])";
  if (!eab_kid.empty() && !eab_hmac.empty()) {
    auto jwk = jwk_json();
    if (!jwk) return std::unexpected(jwk.error());
    const std::string protected_json = R"({"alg":"HS256","kid":")" + json_escape(eab_kid) + R"(","url":")" + json_escape(new_account_) + "\"}";
    const std::string signing_input = b64(protected_json) + "." + b64(*jwk);
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_size = 0;
    HMAC(EVP_sha256(), eab_hmac.data(), static_cast<int>(eab_hmac.size()),
         reinterpret_cast<const unsigned char*>(signing_input.data()), signing_input.size(), mac, &mac_size);
    payload += R"(,"externalAccountBinding":{"protected":")" + b64(protected_json) + R"(","payload":")" + b64(*jwk) +
               R"(","signature":")" + b64({reinterpret_cast<char*>(mac), mac_size}) + "\"}";
  }
  payload += "}";
  if (auto status = register_with(payload, location); !status) return status;
  kid_ = location;
  if (kid_.empty()) return fail(Errc::Io, "the ACME server gave no account URL");
  return {};
}

}  // namespace campfire::net::front
