// Rails: reference/lib/restricted_http/private_network_guard.rb, surfguard. Rust: crates/campfire/src/integrations/
// net/guard.rs.
#include "app/network_guard.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace campfire::app {

namespace {

std::uint32_t v4_value(const unsigned char* a) {
  return (std::uint32_t{a[0]} << 24) | (std::uint32_t{a[1]} << 16) | (std::uint32_t{a[2]} << 8) | a[3];
}

bool in_v4(std::uint32_t address, std::uint32_t network, int prefix) {
  const std::uint32_t mask = prefix == 0 ? 0 : 0xFFFFFFFFU << (32 - prefix);
  return (address & mask) == (network & mask);
}

bool blocked_v4(std::uint32_t a) {
  struct Range {
    std::uint32_t network;
    int prefix;
  };
  static constexpr std::array<Range, 16> kRanges = {{
      {0x00000000U, 8},   // "this" network
      {0x0A000000U, 8},   // 10/8
      {0x64400000U, 10},  // 100.64/10 shared address space
      {0x7F000000U, 8},   // loopback
      {0xA9FE0000U, 16},  // link local
      {0xAC100000U, 12},  // 172.16/12
      {0xC0000000U, 24},  // 192.0.0.0/24
      {0xC0000200U, 24},  // TEST-NET-1
      {0xC0586300U, 24},  // 192.88.99/24
      {0xC0A80000U, 16},  // 192.168/16
      {0xC6120000U, 15},  // 198.18/15 benchmarking
      {0xC6336400U, 24},  // TEST-NET-2
      {0xCB007100U, 24},  // TEST-NET-3
      {0xE0000000U, 4},   // multicast
      {0xF0000000U, 4},   // reserved
      {0xFFFFFFFFU, 32},  // broadcast
  }};
  return std::any_of(kRanges.begin(), kRanges.end(), [&](const Range& r) { return in_v4(a, r.network, r.prefix); });
}

bool blocked_v6(const unsigned char* b) {
  const auto prefix_is = [&](std::initializer_list<unsigned char> bytes) {
    std::size_t i = 0;
    for (const unsigned char v : bytes) {
      if (b[i++] != v) return false;
    }
    return true;
  };
  const bool all_zero_to_15 = std::all_of(b, b + 15, [](unsigned char c) { return c == 0; });
  if (all_zero_to_15 && (b[15] == 0 || b[15] == 1)) return true;  // :: and ::1
  // An IPv4 mapped address: the rules of the IPv4 address.
  if (prefix_is({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF})) return blocked_v4(v4_value(b + 12));
  if (prefix_is({0x00, 0x64, 0xFF, 0x9B, 0, 0, 0, 0, 0, 0, 0, 0})) return blocked_v4(v4_value(b + 12));  // NAT64
  if (prefix_is({0x01, 0x00, 0, 0, 0, 0, 0, 0})) return true;  // 100::/64 discard
  if (prefix_is({0x20, 0x01, 0x0D, 0xB8})) return true;        // documentation
  if ((b[0] & 0xFE) == 0xFC) return true;                      // fc00::/7
  if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return true;      // fe80::/10
  if (b[0] == 0xFF) return true;                               // multicast
  return false;
}

// The host name syntax of surfguard: letters, digits and hyphens in labels of 1 to 63 characters.
bool plain_host_name(std::string_view host) {
  if (host.empty() || host.size() > 255) return false;
  if (host.back() == '.') host.remove_suffix(1);
  std::size_t start = 0;
  while (start <= host.size()) {
    std::size_t dot = host.find('.', start);
    if (dot == std::string_view::npos) dot = host.size();
    const std::string_view label = host.substr(start, dot - start);
    if (label.empty() || label.size() > 63) return false;
    const auto alnum = [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    if (!alnum(label.front()) || !alnum(label.back())) return false;
    if (!std::all_of(label.begin(), label.end(), [&](char c) { return alnum(c) || c == '-'; })) return false;
    start = dot + 1;
    if (dot == host.size()) break;
  }
  return true;
}

}  // namespace

bool blocked_address(std::string_view bytes) {
  if (bytes.size() == 4) return blocked_v4(v4_value(reinterpret_cast<const unsigned char*>(bytes.data())));
  if (bytes.size() == 16) return blocked_v6(reinterpret_cast<const unsigned char*>(bytes.data()));
  return true;
}

std::optional<std::string> resolve_public_address(std::string_view host, const HostLookup& lookup) {
  if (!plain_host_name(host)) return std::nullopt;
  std::vector<std::string> v4;
  std::vector<std::string> v6;
  for (const std::string& bytes : lookup(std::string(host))) {
    if (blocked_address(bytes)) continue;
    char text[INET6_ADDRSTRLEN] = {};
    if (bytes.size() == 4) {
      if (inet_ntop(AF_INET, bytes.data(), text, sizeof text) != nullptr) v4.emplace_back(text);
    } else if (bytes.size() == 16) {
      if (inet_ntop(AF_INET6, bytes.data(), text, sizeof text) != nullptr) v6.emplace_back(text);
    }
  }
  if (!v4.empty()) return v4.front();
  if (!v6.empty()) return v6.front();
  return std::nullopt;
}

std::optional<std::string> resolve_public_address(std::string_view host) {
  return resolve_public_address(host, [](const std::string& name) {
    std::vector<std::string> out;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &found) != 0 || found == nullptr) return out;
    for (const addrinfo* item = found; item != nullptr; item = item->ai_next) {
      if (item->ai_family == AF_INET) {
        const auto* sin = reinterpret_cast<const sockaddr_in*>(item->ai_addr);
        out.emplace_back(reinterpret_cast<const char*>(&sin->sin_addr), 4);
      } else if (item->ai_family == AF_INET6) {
        const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(item->ai_addr);
        out.emplace_back(reinterpret_cast<const char*>(&sin6->sin6_addr), 16);
      }
    }
    freeaddrinfo(found);
    return out;
  });
}

}  // namespace campfire::app
