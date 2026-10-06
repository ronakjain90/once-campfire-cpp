// Automatic certificates, as Thruster gets them from autocert. Rust: crates/kit/src/front/acme.rs.
//
// On the first TLS handshake for a TLS_DOMAIN name, an ECDSA P-256 certificate is ordered from
// ACME_DIRECTORY. The order answers TLS-ALPN-01 on the HTTPS port, or HTTP-01 on the HTTP port
// when that fails. The certificate is renewed 30 days before it expires. The certificates and the
// account key live in STORAGE_PATH in the layout of autocert's `DirCache`: "<domain>" has the
// PEM private key, then the PEM chain, and "acme_account+key" has the PEM key of the account.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/config.hpp"
#include "net/front/tls_keys.hpp"

namespace campfire::net::front {

class AcmeAccount;

enum class ChallengeType : std::uint8_t { TlsAlpn01, Http01 };

struct AcmeOptions {
  std::string directory_url;
  std::string eab_kid;       // EAB_KID
  std::string eab_hmac_key;  // EAB_HMAC_KEY, unpadded URL-safe base64
  std::filesystem::path storage_path;
  std::vector<std::string> domains;
  std::vector<ChallengeType> challenge_types{ChallengeType::TlsAlpn01, ChallengeType::Http01};
  std::string directory_root;  // a PEM file of roots for the ACME directory (test CAs)
  int http_timeout_seconds = 30;
  // For tests: how long the renewal waits is `not_after - renew_before`, at least this many ms.
  std::chrono::seconds renew_before = std::chrono::hours(30 * 24);
  std::chrono::seconds poll_interval = std::chrono::seconds(1);

  [[nodiscard]] static AcmeOptions from_config(const FrontConfig& config);
};

class CertManager {
 public:
  explicit CertManager(AcmeOptions options);
  ~CertManager();
  CertManager(const CertManager&) = delete;
  CertManager& operator=(const CertManager&) = delete;

  // `HostWhitelist`: a TLS_DOMAIN name, in ASCII and lowercase.
  [[nodiscard]] bool host_allowed(std::string_view host) const;

  enum class State : std::uint8_t { Ready, Pending, Failed };
  // The certificate for the server name of a handshake (`GetCertificate`). Ready: `out` is set.
  // Pending: an order runs in the background thread; ask again later. Failed: `error` says why.
  [[nodiscard]] State certificate(std::string_view server_name, std::shared_ptr<const CertifiedKey>& out,
                                  std::string& error);

  // The certificate that a TLS-ALPN-01 validation handshake for `server_name` gets.
  [[nodiscard]] std::shared_ptr<const CertifiedKey> challenge_certificate_for(std::string_view server_name) const;
  // The certificate in memory for a normalized name.
  [[nodiscard]] std::shared_ptr<const CertifiedKey> loaded(std::string_view name) const;
  // The HTTP-01 answer for a "/.well-known/acme-challenge/<token>" path.
  [[nodiscard]] std::optional<std::string> http_token(std::string_view path) const;
  // Sets the HTTP-01 answer for a path (an order does this, and a test).
  void set_http_token(const std::string& path, std::string key_authorization);

  // autocert's server name checks and normalization. Returns the error text in `error`.
  [[nodiscard]] static std::string server_name_to_domain(std::string_view server_name, std::string& error);

 private:
  struct Attempt {
    bool done = false;
    bool ok = false;
    std::string error;
    std::chrono::steady_clock::time_point finished{};
  };

  void run();
  Status obtain_now(const std::string& name);
  Status issue(const std::string& name, std::shared_ptr<CertifiedKey>& out);
  Status order(const std::string& name, ChallengeType type, std::shared_ptr<CertifiedKey>& out);
  void install(const std::string& name, std::shared_ptr<const CertifiedKey> certificate);
  [[nodiscard]] bool has_cache_file(const std::string& name) const;
  [[nodiscard]] std::shared_ptr<CertifiedKey> read_cached(const std::string& name) const;

  AcmeOptions options_;
  std::set<std::string> allowed_;

  mutable std::shared_mutex mutex_;  // the maps below
  std::map<std::string, std::shared_ptr<const CertifiedKey>> certificates_;
  std::map<std::string, std::shared_ptr<const CertifiedKey>> challenge_certificates_;
  std::map<std::string, std::string> http_tokens_;

  std::mutex work_mutex_;  // the queue, the attempts and the renewals
  std::condition_variable work_cv_;
  std::vector<std::string> queue_;
  std::map<std::string, Attempt> attempts_;
  std::map<std::string, std::chrono::system_clock::time_point> renewals_;
  bool stopping_ = false;
  std::thread thread_;

  std::unique_ptr<AcmeAccount> account_;  // used by the thread only
};

}  // namespace campfire::net::front
