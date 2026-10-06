// Automatic certificates. Rust: crates/kit/src/front/acme.rs.
#include "net/front/acme.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <fstream>
#include <random>
#include <sstream>

#include "compat/base64.hpp"
#include "core/log.hpp"
#include "net/front/acme_account.hpp"

namespace campfire::net::front {

namespace {

constexpr auto kRenewJitter = std::chrono::hours(1);  // autocert's renewJitter
constexpr auto kFailureMemory = std::chrono::seconds(5);

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

}  // namespace

AcmeOptions AcmeOptions::from_config(const FrontConfig& config) {
  AcmeOptions options;
  options.directory_url = config.acme_directory_url;
  options.eab_kid = config.eab_kid;
  options.eab_hmac_key = config.eab_hmac_key;
  options.storage_path = config.storage_path;
  options.domains = config.tls_domains;
  return options;
}

CertManager::CertManager(AcmeOptions options) : options_(std::move(options)) {
  for (const std::string& domain : options_.domains) {
    if (std::string name = normalize_domain(domain); !name.empty()) allowed_.insert(std::move(name));
  }
  thread_ = std::thread([this] { run(); });
}

CertManager::~CertManager() {
  {
    const std::lock_guard lock(work_mutex_);
    stopping_ = true;
  }
  work_cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

bool CertManager::host_allowed(std::string_view host) const { return allowed_.contains(std::string(host)); }

std::string CertManager::server_name_to_domain(std::string_view server_name, std::string& error) {
  if (server_name.empty()) {
    error = "acme/autocert: missing server name";
    return {};
  }
  std::string_view trimmed = server_name;
  while (trimmed.starts_with('.')) trimmed.remove_prefix(1);
  while (trimmed.ends_with('.')) trimmed.remove_suffix(1);
  if (trimmed.find('.') == std::string_view::npos) {
    error = "acme/autocert: server name component count invalid";
    return {};
  }
  if (server_name.find_first_of("/\\") != std::string_view::npos) {
    error = "acme/autocert: server name contains invalid character";
    return {};
  }
  std::string_view host = server_name;
  while (host.ends_with('.')) host.remove_suffix(1);
  std::string name = normalize_domain(host);
  if (name.empty()) error = "acme/autocert: invalid server name";
  return name;
}

std::shared_ptr<const CertifiedKey> CertManager::loaded(std::string_view name) const {
  const std::shared_lock lock(mutex_);
  const auto found = certificates_.find(std::string(name));
  return found == certificates_.end() ? nullptr : found->second;
}

std::shared_ptr<const CertifiedKey> CertManager::challenge_certificate_for(std::string_view server_name) const {
  std::string error;
  const std::string name = server_name_to_domain(server_name, error);
  if (name.empty()) return nullptr;
  const std::shared_lock lock(mutex_);
  const auto found = challenge_certificates_.find(name);
  return found == challenge_certificates_.end() ? nullptr : found->second;
}

std::optional<std::string> CertManager::http_token(std::string_view path) const {
  const std::shared_lock lock(mutex_);
  const auto found = http_tokens_.find(std::string(path));
  if (found == http_tokens_.end()) return std::nullopt;
  return found->second;
}

bool CertManager::has_cache_file(const std::string& name) const {
  std::error_code ec;
  return std::filesystem::exists(options_.storage_path / name, ec);
}

std::shared_ptr<CertifiedKey> CertManager::read_cached(const std::string& name) const {
  const std::string pem = read_file(options_.storage_path / name);
  if (pem.empty()) return nullptr;
  auto parsed = parse_cached(pem, name, std::time(nullptr));
  if (!parsed) {
    log_info("TLS: ignoring cached certificate domain={} error={}", name, parsed.error().message);
    return nullptr;
  }
  return *parsed;
}

CertManager::State CertManager::certificate(std::string_view server_name, std::shared_ptr<const CertifiedKey>& out,
                                            std::string& error) {
  const std::string name = server_name_to_domain(server_name, error);
  if (name.empty()) return State::Failed;
  if (auto found = loaded(name)) {
    out = std::move(found);
    return State::Ready;
  }
  // A cached certificate serves even a name that TLS_DOMAIN no longer has, as autocert does. Any
  // other name is turned away here, before it costs an entry in the table of attempts.
  if (!host_allowed(name) && !has_cache_file(name)) {
    error = "acme/autocert: host \"" + name + "\" not configured in HostWhitelist";
    return State::Failed;
  }
  const std::lock_guard lock(work_mutex_);
  const auto now = std::chrono::steady_clock::now();
  if (const auto it = attempts_.find(name); it != attempts_.end()) {
    Attempt& attempt = it->second;
    if (!attempt.done) return State::Pending;
    if (attempt.ok) {
      if (auto found = loaded(name)) {
        out = std::move(found);
        return State::Ready;
      }
    } else if (now - attempt.finished < kFailureMemory) {
      error = attempt.error;
      return State::Failed;
    }
  }
  attempts_[name] = Attempt{};
  queue_.push_back(name);
  work_cv_.notify_all();
  return State::Pending;
}

void CertManager::install(const std::string& name, std::shared_ptr<const CertifiedKey> certificate) {
  const std::time_t not_after = certificate->not_after;
  {
    const std::unique_lock lock(mutex_);
    certificates_[name] = std::move(certificate);
  }
  // autocert's `domainRenewal`: `RenewBefore` ahead of the end, less a jitter.
  static thread_local std::mt19937_64 random{std::random_device{}()};
  const auto jitter = std::chrono::seconds(static_cast<std::int64_t>(
      std::uniform_real_distribution<double>(0.0, 1.0)(random) * static_cast<double>(std::chrono::seconds(kRenewJitter).count())));
  const auto end = std::chrono::system_clock::from_time_t(not_after);
  const std::lock_guard lock(work_mutex_);
  renewals_[name] = end - options_.renew_before - jitter;
}

Status CertManager::obtain_now(const std::string& name) {
  if (loaded(name)) return {};
  if (auto cached = read_cached(name)) {
    install(name, cached);
    return {};
  }
  if (!host_allowed(name)) return fail(Errc::Config, "acme/autocert: host \"" + name + "\" not configured in HostWhitelist");
  std::shared_ptr<CertifiedKey> issued;
  if (auto status = issue(name, issued); !status) return status;
  install(name, issued);
  return {};
}

void CertManager::run() {
  std::unique_lock lock(work_mutex_);
  while (!stopping_) {
    const auto system_now = std::chrono::system_clock::now();
    std::string job;
    bool renewal = false;
    if (!queue_.empty()) {
      job = queue_.front();
      queue_.erase(queue_.begin());
    } else {
      auto next = std::chrono::system_clock::time_point::max();
      for (const auto& [name, when] : renewals_) {
        if (when < next) next = when;
        if (when <= system_now && job.empty()) {
          job = name;
          renewal = true;
        }
      }
      if (job.empty()) {
        if (next == std::chrono::system_clock::time_point::max()) {
          work_cv_.wait(lock);
        } else {
          work_cv_.wait_until(lock, next);
        }
        continue;
      }
    }
    lock.unlock();
    Status status;
    if (renewal) {
      std::shared_ptr<CertifiedKey> issued;
      status = issue(job, issued);
      if (status) {
        install(job, issued);
      } else {
        log_error("TLS: certificate renewal failed domain={} error={}", job, status.error().message);
        static thread_local std::mt19937_64 random{std::random_device{}()};
        const auto half = std::chrono::seconds(kRenewJitter).count() / 2;
        const auto wait = std::chrono::seconds(half + static_cast<std::int64_t>(std::uniform_real_distribution<double>(0.0, 1.0)(random) * static_cast<double>(half)));
        const std::lock_guard guard(work_mutex_);
        renewals_[job] = std::chrono::system_clock::now() + wait;
      }
    } else {
      status = obtain_now(job);
    }
    lock.lock();
    if (!renewal) {
      Attempt& attempt = attempts_[job];
      attempt.done = true;
      attempt.ok = status.has_value();
      attempt.error = status ? std::string() : status.error().message;
      attempt.finished = std::chrono::steady_clock::now();
    }
  }
}

}  // namespace campfire::net::front
