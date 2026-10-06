// Proxy-aware request facts. Rails: ActionDispatch::RemoteIp, Rack::Request. Rust: crates/kit/src/request.rs.
#include "app/proxy.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <charconv>

namespace campfire::app {

namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

bool all_digits(std::string_view s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// `value.trim().split([',', ' ', '\t']).filter(non empty)`
std::vector<std::string_view> split_header(std::string_view value) {
  std::vector<std::string_view> out;
  value = trim(value);
  while (!value.empty()) {
    const std::size_t at = value.find_first_of(", \t");
    const std::string_view part = value.substr(0, at);
    if (!part.empty()) out.push_back(part);
    if (at == std::string_view::npos) break;
    value.remove_prefix(at + 1);
  }
  return out;
}

// `Rack::Utils.forwarded_values(header)[param]`
std::vector<std::string> forwarded_values(std::string_view header, std::string_view param) {
  std::vector<std::string> values;
  while (!header.empty()) {
    const std::size_t at = header.find_first_of(",;");
    const std::string_view element = header.substr(0, at);
    header = at == std::string_view::npos ? std::string_view{} : header.substr(at + 1);
    const std::size_t eq = element.find('=');
    if (eq == std::string_view::npos) continue;
    if (!net::iequals(trim(element.substr(0, eq)), param)) continue;
    std::string_view value = trim(element.substr(eq + 1));
    while (!value.empty() && value.front() == '"') value.remove_prefix(1);
    while (!value.empty() && value.back() == '"') value.remove_suffix(1);
    values.emplace_back(value);
  }
  return values;
}

bool is_scheme(std::string_view s) { return s == "https" || s == "http" || s == "wss" || s == "ws"; }

bool scheme_is_https(const net::Request& r) {
  if (r.header("x-forwarded-ssl") == "on") return true;
  if (const std::string_view forwarded = r.header("forwarded"); !forwarded.empty()) {
    const auto protos = forwarded_values(forwarded, "proto");
    if (!protos.empty() && is_scheme(protos.back())) return protos.back() == "https" || protos.back() == "wss";
  }
  for (const std::string_view name : {"x-forwarded-proto", "x-forwarded-scheme"}) {
    const std::string_view value = r.header(name);
    if (value.empty()) continue;
    const auto parts = split_header(value);
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
      if (is_scheme(*it)) return *it == "https" || *it == "wss";
    }
  }
  return false;
}

// `Rack::Request#split_authority(...)[1]`: strips a port and the brackets of IPv6.
std::string authority_address(std::string_view authority) {
  authority = trim(authority);
  if (authority.starts_with('[')) {
    authority.remove_prefix(1);
    return std::string(authority.substr(0, authority.find(']')));
  }
  if (IpAddress::parse(authority)) return std::string(authority);
  const std::size_t colon = authority.rfind(':');
  if (colon != std::string_view::npos && all_digits(authority.substr(colon + 1)) &&
      authority.substr(0, colon).find(':') == std::string_view::npos) {
    return std::string(authority.substr(0, colon));
  }
  return std::string(authority);
}

std::vector<IpAddress> sanitize_ips(const std::vector<std::string>& texts) {
  std::vector<IpAddress> out;
  for (std::string_view t : texts) {
    while (!t.empty() && (t.front() == '[' || t.front() == ']')) t.remove_prefix(1);
    while (!t.empty() && (t.back() == '[' || t.back() == ']')) t.remove_suffix(1);
    if (auto ip = IpAddress::parse(t)) out.push_back(*ip);
  }
  return out;
}

std::vector<std::string> forwarded_for(const net::Request& r) {
  if (const std::string_view forwarded = r.header("forwarded"); !forwarded.empty()) {
    const auto values = forwarded_values(forwarded, "for");
    if (!values.empty()) {
      std::vector<std::string> out;
      for (const auto& v : values) out.push_back(authority_address(v));
      return out;
    }
  }
  std::vector<std::string> out;
  for (const std::string_view part : split_header(r.header("x-forwarded-for"))) out.push_back(authority_address(part));
  return out;
}

}  // namespace

std::optional<IpAddress> IpAddress::parse(std::string_view text) {
  const std::string z(text);
  IpAddress ip;
  if (inet_pton(AF_INET, z.c_str(), ip.bytes.data()) == 1) return ip;
  ip = IpAddress{};
  ip.v6 = true;
  if (inet_pton(AF_INET6, z.c_str(), ip.bytes.data()) == 1) return ip;
  return std::nullopt;
}

std::string IpAddress::to_string() const {
  char buffer[INET6_ADDRSTRLEN] = {};
  inet_ntop(v6 ? AF_INET6 : AF_INET, bytes.data(), buffer, sizeof buffer);
  return buffer;
}

std::optional<IpNet> IpNet::parse(std::string_view text) {
  const std::size_t slash = text.find('/');
  auto address = IpAddress::parse(text.substr(0, slash));
  if (!address) return std::nullopt;
  int prefix = address->v6 ? 128 : 32;
  if (slash != std::string_view::npos) {
    const std::string_view digits = text.substr(slash + 1);
    int value = 0;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (ec != std::errc{} || end != digits.data() + digits.size() || value < 0 || value > prefix) return std::nullopt;
    prefix = value;
  }
  return IpNet{*address, prefix};
}

bool IpNet::contains(const IpAddress& ip) const noexcept {
  if (ip.v6 != address.v6) return false;
  const int full = prefix / 8;
  for (int i = 0; i < full; ++i) {
    if (address.bytes[static_cast<std::size_t>(i)] != ip.bytes[static_cast<std::size_t>(i)]) return false;
  }
  const int rest = prefix % 8;
  if (rest == 0) return true;
  const auto mask = static_cast<std::uint8_t>(0xFFU << (8 - rest));
  const auto at = static_cast<std::size_t>(full);
  return (address.bytes[at] & mask) == (ip.bytes[at] & mask);
}

std::vector<IpNet> default_trusted_proxies() {
  std::vector<IpNet> out;
  for (const char* text : {"127.0.0.0/8", "::1/128", "fc00::/7", "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16",
                           "169.254.0.0/16", "fe80::/10"}) {
    out.push_back(*IpNet::parse(text));
  }
  return out;
}

ProxyConfig ProxyConfig::production(bool disable_ssl) {
  ProxyConfig config;
  config.assume_ssl = !disable_ssl;
  config.force_ssl = !disable_ssl;
  return config;
}

RequestInfo::RequestInfo(const net::Request& request, const ProxyConfig& proxy)
    : request_(&request), proxy_(&proxy), ssl_(proxy.assume_ssl || scheme_is_https(request)) {}

std::string RequestInfo::raw_host_with_port() const {
  const std::string_view forwarded = request_->header("x-forwarded-host");
  if (!trim(forwarded).empty()) {
    const std::size_t comma = forwarded.rfind(',');
    std::string_view last = comma == std::string_view::npos ? forwarded : forwarded.substr(comma + 1);
    while (!last.empty() && (last.front() == ' ' || last.front() == '\t')) last.remove_prefix(1);
    return std::string(last);
  }
  if (request_->has_header("host")) return std::string(request_->header("host"));
  return "localhost";
}

std::string RequestInfo::host() const {
  std::string raw = raw_host_with_port();
  const std::size_t colon = raw.rfind(':');
  if (colon != std::string::npos && all_digits(std::string_view(raw).substr(colon + 1))) raw.resize(colon);
  return raw;
}

int RequestInfo::port() const {
  const std::string raw = raw_host_with_port();
  const int standard = ssl_ ? 443 : 80;
  const std::size_t colon = raw.rfind(':');
  if (colon == std::string::npos) return standard;
  const std::string_view digits = std::string_view(raw).substr(colon + 1);
  if (!all_digits(digits)) return standard;
  unsigned value = 0;
  const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
  return ec == std::errc{} && value <= 65535 ? static_cast<int>(value) : standard;
}

std::string RequestInfo::host_with_port() const {
  const int p = port();
  return p == (ssl_ ? 443 : 80) ? host() : host() + ":" + std::to_string(p);
}

std::string RequestInfo::base_url() const { return std::string(protocol()) + host_with_port(); }

std::string RequestInfo::fullpath() const {
  std::string out(request_->path);
  if (!request_->query.empty()) out += "?" + std::string(request_->query);
  return out;
}

std::string RequestInfo::url() const { return base_url() + fullpath(); }

std::optional<std::string> RequestInfo::remote_ip() const {
  const auto peer = IpAddress::parse(request_->remote_ip);
  auto client_ips = sanitize_ips([&] {
    std::vector<std::string> v;
    for (const auto part : split_header(request_->header("client-ip"))) v.emplace_back(part);
    return v;
  }());
  std::reverse(client_ips.begin(), client_ips.end());
  auto forwarded_ips = sanitize_ips(forwarded_for(*request_));
  std::reverse(forwarded_ips.begin(), forwarded_ips.end());
  if (proxy_->ip_spoofing_check && !client_ips.empty() && !forwarded_ips.empty() &&
      std::find(forwarded_ips.begin(), forwarded_ips.end(), client_ips.back()) == forwarded_ips.end()) {
    return std::nullopt;
  }
  std::vector<IpAddress> ips = forwarded_ips;
  ips.insert(ips.end(), client_ips.begin(), client_ips.end());
  const auto trusted = [&](const IpAddress& ip) {
    return std::any_of(proxy_->trusted_proxies.begin(), proxy_->trusted_proxies.end(),
                       [&](const IpNet& net) { return net.contains(ip); });
  };
  std::vector<IpAddress> candidates = ips;
  if (peer) candidates.push_back(*peer);
  for (const IpAddress& ip : candidates) {
    if (!trusted(ip)) return ip.to_string();
  }
  if (!ips.empty()) return ips.back().to_string();
  if (peer) return peer->to_string();
  return std::string{};
}

}  // namespace campfire::app
