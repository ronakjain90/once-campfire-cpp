// The order of a certificate (RFC 8555, section 7.4). Rust: crates/kit/src/front/acme.rs (issue, order).
#include <openssl/sha.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#include "compat/base64.hpp"
#include "core/log.hpp"
#include "net/front/acme.hpp"
#include "net/front/acme_account.hpp"

namespace campfire::net::front {

namespace {

constexpr std::string_view kAccountKey = "acme_account+key";
constexpr auto kOrderTimeout = std::chrono::seconds(120);

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

}  // namespace

Status CertManager::issue(const std::string& name, std::shared_ptr<CertifiedKey>& out) {
  if (!account_) {
    account_ =
        std::make_unique<AcmeAccount>(options_.directory_url, options_.directory_root, options_.http_timeout_seconds);
  }
  if (!account_->has_key()) {
    const std::string pem = read_file(options_.storage_path / std::string(kAccountKey));
    bool existing = false;
    PKeyPtr key;
    if (!pem.empty()) {
      if (auto parsed = parse_private_key(pem)) {
        key = std::move(*parsed);
        existing = true;
      }
    }
    if (!key) {
      auto generated = generate_p256_key();
      if (!generated) return std::unexpected(generated.error());
      key = std::move(*generated);
      auto text = private_key_pem(key.get());
      if (!text) return std::unexpected(text.error());
      if (auto status = write_cache_file(options_.storage_path, kAccountKey, *text); !status) return status;
    }
    account_->set_key(std::move(key), existing);
    std::string hmac;
    if (!options_.eab_kid.empty() && !options_.eab_hmac_key.empty()) {
      if (auto decoded = compat::base64::urlsafe_decode(options_.eab_hmac_key)) {
        hmac = std::move(*decoded);
      } else {
        log_error("Error decoding EAB_HMACKey");
      }
    }
    if (auto status = account_->register_account(hmac.empty() ? std::string() : options_.eab_kid, hmac); !status) {
      account_->set_key(nullptr, false);
      return status;
    }
  }
  Error last{Errc::Internal, "no supported challenge type"};
  for (const ChallengeType type : options_.challenge_types) {
    auto status = order(name, type, out);
    if (status) {
      log_info("TLS: obtained certificate domain={}", name);
      return {};
    }
    log_info("TLS: order failed domain={} challenge={} error={}", name,
             type == ChallengeType::TlsAlpn01 ? "tls-alpn-01" : "http-01", status.error().message);
    last = status.error();
  }
  return std::unexpected(last);
}

Status CertManager::order(const std::string& name, ChallengeType type, std::shared_ptr<CertifiedKey>& out) {
  AcmeAccount& account = *account_;
  std::string order_url;
  auto order = account.post_json(account.new_order_url(),
                                 R"({"identifiers":[{"type":"dns","value":")" + name + R"("}]})", false, &order_url);
  if (!order) return std::unexpected(order.error());

  // The challenge answers live until the order is done.
  struct Provisioned {
    CertManager* manager;
    std::vector<std::string> paths;
    std::vector<std::string> domains;
    ~Provisioned() {
      const std::unique_lock lock(manager->mutex_);
      for (const std::string& path : paths) manager->http_tokens_.erase(path);
      for (const std::string& domain : domains) manager->challenge_certificates_.erase(domain);
    }
  } provisioned{this, {}, {}};

  const std::string_view wanted = type == ChallengeType::TlsAlpn01 ? "tls-alpn-01" : "http-01";
  for (const Json& authorization_url :
       order->find("authorizations") != nullptr ? order->find("authorizations")->array() : Json::Array{}) {
    auto authorization = account.post_json(std::string(authorization_url.as_string()), {}, true);
    if (!authorization) return std::unexpected(authorization.error());
    const std::string_view status = authorization->str("status");
    if (status == "valid") continue;
    if (status != "pending") return fail(Errc::Io, "authorization is " + std::string(status));
    const Json* identifier = authorization->find("identifier");
    const std::string domain(identifier != nullptr ? identifier->str("value") : std::string_view{});
    const Json* challenges = authorization->find("challenges");
    const Json* chosen = nullptr;
    if (challenges != nullptr) {
      for (const Json& c : challenges->array()) {
        if (c.str("type") == wanted) chosen = &c;
      }
    }
    if (chosen == nullptr) return fail(Errc::Io, "challenge type not offered");
    const std::string token(chosen->str("token"));
    const std::string key_authorization = account.key_authorization(token);
    if (type == ChallengeType::TlsAlpn01) {
      unsigned char digest[SHA256_DIGEST_LENGTH];
      SHA256(reinterpret_cast<const unsigned char*>(key_authorization.data()), key_authorization.size(), digest);
      auto certificate = challenge_certificate(domain, {reinterpret_cast<char*>(digest), sizeof digest});
      if (!certificate) return std::unexpected(certificate.error());
      const std::unique_lock lock(mutex_);
      challenge_certificates_[domain] = *certificate;
      provisioned.domains.push_back(domain);
    } else {
      const std::string path = "/.well-known/acme-challenge/" + token;
      set_http_token(path, key_authorization);
      provisioned.paths.push_back(path);
    }
    if (auto ready = account.post_json(std::string(chosen->str("url")), "{}", false); !ready)
      return std::unexpected(ready.error());
  }

  // Polls the order until it leaves "pending" and "processing", at most 2 minutes.
  const auto poll = [&](std::string_view goal) -> Result<Json> {
    const auto deadline = std::chrono::steady_clock::now() + kOrderTimeout;
    while (true) {
      auto current = account.post_json(order_url, {}, true);
      if (!current) return current;
      const std::string_view status = current->str("status");
      if (status == goal || (goal == "ready" && status == "valid")) return current;
      if (status == "invalid")
        return fail(Errc::Io,
                    "order is invalid: " + std::string(current->find("error") != nullptr ? "see the ACME server" : ""));
      if (std::chrono::steady_clock::now() > deadline)
        return fail(Errc::Timeout, "the order did not become " + std::string(goal));
      std::this_thread::sleep_for(options_.poll_interval);
    }
  };
  auto ready = poll("ready");
  if (!ready) return std::unexpected(ready.error());

  auto key = generate_p256_key();
  if (!key) return std::unexpected(key.error());
  auto csr = make_csr(name, key->get());
  if (!csr) return std::unexpected(csr.error());
  const std::string finalize(order->str("finalize"));
  if (ready->str("status") != "valid") {
    if (auto finalized =
            account.post_json(finalize, R"({"csr":")" + compat::base64::urlsafe_encode_unpadded(*csr) + "\"}", false);
        !finalized) {
      return std::unexpected(finalized.error());
    }
  }
  auto valid = poll("valid");
  if (!valid) return std::unexpected(valid.error());
  const std::string certificate_url(valid->str("certificate"));
  if (certificate_url.empty()) return fail(Errc::Io, "the order has no certificate");
  auto chain = account.post(certificate_url, {}, true);
  if (!chain) return std::unexpected(chain.error());
  if (chain->status != 200) return fail(Errc::Io, "certificate download answered " + std::to_string(chain->status));

  auto entry = cache_entry(key->get(), chain->body);
  if (!entry) return std::unexpected(entry.error());
  auto parsed = parse_cached(*entry, name, std::time(nullptr));
  if (!parsed) return std::unexpected(parsed.error());
  if (auto status = write_cache_file(options_.storage_path, name, *entry); !status) return status;
  out = *parsed;
  return {};
}

}  // namespace campfire::net::front
