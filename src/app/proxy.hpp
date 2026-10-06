// Request facts that Rails reads from proxy headers: protocol, host, port, base URL, remote IP.
// Rails: ActionDispatch::Request, ActionDispatch::RemoteIp, ActionDispatch::AssumeSSL.
// Rust: crates/kit/src/request.rs.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/config.hpp"
#include "net/http.hpp"

namespace campfire::app {

// An IPv4 or IPv6 address, or a network with a prefix length.
struct IpAddress {
  bool v6 = false;
  std::array<std::uint8_t, 16> bytes{};
  [[nodiscard]] static std::optional<IpAddress> parse(std::string_view text);
  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] bool operator==(const IpAddress&) const = default;
};

struct IpNet {
  IpAddress address;
  int prefix = 0;
  [[nodiscard]] static std::optional<IpNet> parse(std::string_view text);
  [[nodiscard]] bool contains(const IpAddress& ip) const noexcept;
};

// `ActionDispatch::RemoteIp::TRUSTED_PROXIES`.
[[nodiscard]] std::vector<IpNet> default_trusted_proxies();

struct ProxyConfig {
  std::vector<IpNet> trusted_proxies = default_trusted_proxies();
  bool ip_spoofing_check = true;
  bool assume_ssl = false;  // `config.assume_ssl`
  bool force_ssl = false;   // `config.force_ssl`
  std::string hsts = "max-age=63072000; includeSubDomains";

  // production.rb: `assume_ssl` and `force_ssl` unless DISABLE_SSL is set.
  [[nodiscard]] static ProxyConfig production(bool disable_ssl);
};

// The facts of one request. The object keeps views of the request: it lives no longer than the request.
class RequestInfo {
 public:
  RequestInfo(const net::Request& request, const ProxyConfig& proxy);

  [[nodiscard]] bool ssl() const noexcept { return ssl_; }
  [[nodiscard]] std::string_view protocol() const noexcept { return ssl_ ? "https://" : "http://"; }
  // `request.host`: X-Forwarded-Host aware, port removed.
  [[nodiscard]] std::string host() const;
  [[nodiscard]] int port() const;
  [[nodiscard]] std::string host_with_port() const;
  [[nodiscard]] std::string base_url() const;
  [[nodiscard]] std::string fullpath() const;
  [[nodiscard]] std::string url() const;
  // `request.remote_ip`. Nothing means IpSpoofAttackError.
  [[nodiscard]] std::optional<std::string> remote_ip() const;

 private:
  [[nodiscard]] std::string raw_host_with_port() const;

  const net::Request* request_;
  const ProxyConfig* proxy_;
  bool ssl_;
};

}  // namespace campfire::app
