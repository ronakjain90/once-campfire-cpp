// `RestrictedHTTP::PrivateNetworkGuard.resolve(hostname)`: a public address for a host name. Rails:
// reference/lib/restricted_http/private_network_guard.rb and the surfguard gem. Rust: crates/campfire/src/
// integrations/net/guard.rs.
//
// This is the part that the push subscription check needs. It blocks the same ranges as surfguard's default policy
// for the cases that matter here; the full guard with the pinned address of the outbound clients is the task of
// the job area (A9).
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace campfire::app {

// True for an address that surfguard's default policy blocks (private, loopback, link local, reserved, multicast).
// `bytes` has 4 bytes (IPv4) or 16 bytes (IPv6).
[[nodiscard]] bool blocked_address(std::string_view bytes);

// The first public address of `host` (IPv4 before IPv6), as text, or nothing if the host does not resolve or only
// resolves to blocked addresses. It blocks: call it on a job thread.
[[nodiscard]] std::optional<std::string> resolve_public_address(std::string_view host);

}  // namespace campfire::app
