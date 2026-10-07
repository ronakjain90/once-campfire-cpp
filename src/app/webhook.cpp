// Rails: app/models/webhook.rb. Rust: crates/campfire/src/integrations/webhook.rs.
#include "app/webhook.hpp"

#include <arpa/inet.h>

#include "app/network_guard.hpp"
#include "richtext/uri.hpp"

namespace campfire::app::webhook {

namespace {

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const auto lower = [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); };
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

bool is_ip_literal(const std::string& host) {
  unsigned char buffer[16];
  return inet_pton(AF_INET, host.c_str(), buffer) == 1 || inet_pton(AF_INET6, host.c_str(), buffer) == 1;
}

// `Net::HTTP.new(host, port)` resolves the name with the system resolver and tries each address in turn.
std::vector<std::string> addresses_of(const unfurl::Network& network, const std::string& host) {
  if (is_ip_literal(host)) return {host};
  const HostLookup lookup = network.lookup ? network.lookup : system_host_lookup();
  std::vector<std::string> out;
  for (const std::string& bytes : lookup(host)) {
    char text[INET6_ADDRSTRLEN] = {};
    if (bytes.size() == 4 && inet_ntop(AF_INET, bytes.data(), text, sizeof text) != nullptr) out.emplace_back(text);
    if (bytes.size() == 16 && inet_ntop(AF_INET6, bytes.data(), text, sizeof text) != nullptr) out.emplace_back(text);
  }
  return out;
}

}  // namespace

Result<Delivery> deliver(const unfurl::Network& network, std::string_view url, std::string payload,
                         std::chrono::seconds deadline) {
  const auto uri = richtext::parse_uri(url);
  if (!uri) return fail(Errc::Parse, "bad URI (is not URI?): \"" + std::string(url) + "\"");
  if (!uri->is_http()) return fail(Errc::Parse, "not an HTTP URI");
  if (!uri->host || uri->host->empty()) return fail(Errc::Parse, "no host component for URI");
  const bool https = uri->scheme && iequals(*uri->scheme, "https");
  if (!uri->port || *uri->port > 65535) return fail(Errc::Parse, "invalid port");
  const auto port = static_cast<std::uint16_t>(*uri->port);
  std::string hostname = *uri->host;
  if (hostname.size() > 1 && hostname.front() == '[' && hostname.back() == ']') {
    hostname = hostname.substr(1, hostname.size() - 2);
  }
  const std::uint16_t default_port = https ? 443 : 80;

  std::string target = uri->path && !uri->path->empty() ? *uri->path : "/";
  if (uri->query) target += "?" + *uri->query;
  unfurl::Request request;
  request.method = "POST";
  request.target = std::move(target);
  request.headers.emplace();
  auto& headers = *request.headers;
  headers.emplace_back("Content-Type", "application/json");
  headers.emplace_back("Accept-Encoding", "gzip;q=1.0,deflate;q=0.6,identity;q=0.3");
  headers.emplace_back("Accept", "*/*");
  headers.emplace_back("User-Agent", "Ruby");
  headers.emplace_back("Host", port == default_port ? hostname : hostname + ":" + std::to_string(port));
  headers.emplace_back("Connection", "close");
  headers.emplace_back("Content-Length", std::to_string(payload.size()));
  request.body = std::move(payload);
  request.decode_content = true;

  const auto addresses = addresses_of(network, hostname);
  if (addresses.empty()) return fail(Errc::Io, "getaddrinfo: " + hostname);
  unfurl::Endpoint endpoint;
  endpoint.https = https;
  endpoint.host = *uri->host;
  endpoint.port = port;
  endpoint.pinned_ip = addresses.front();
  endpoint.more_ips.assign(addresses.begin() + 1, addresses.end());

  unfurl::Timeouts timeouts;
  timeouts.open = std::chrono::duration_cast<std::chrono::milliseconds>(jobs::webhook::kEndpointTimeout);
  timeouts.read = timeouts.open;
  const auto until = unfurl::Clock::now() + deadline;

  // The wait stops a little before the deadline (it counts whole milliseconds): that is the deadline too.
  const auto deadline_hit = [&] { return unfurl::Clock::now() + std::chrono::milliseconds(100) >= until; };
  const auto timed_out = [&](std::chrono::seconds after) {
    return Delivery{std::nullopt, jobs::webhook::timed_out(after)};
  };
  auto response = unfurl::exchange(network, endpoint, request, timeouts, until);
  if (!response) {
    if (response.error().code == Errc::Timeout) {
      // The 7 seconds of a connect or a read, or the deadline of the whole delivery.
      return timed_out(deadline_hit() ? deadline : jobs::webhook::kEndpointTimeout);
    }
    return std::unexpected(response.error());
  }
  const int status = response->status;
  const auto content_type = response->content_type();
  auto body = response->read_body(jobs::webhook::kMaxReplySize);
  if (!body) {
    if (body.error().code == Errc::Timeout) {
      return timed_out(deadline_hit() ? deadline : jobs::webhook::kEndpointTimeout);
    }
    return std::unexpected(body.error());
  }
  if (body->too_large) {
    return fail(Errc::Io,
                "the reply is larger than " + std::to_string(jobs::webhook::kMaxReplySize / 1024 / 1024) + " MB");
  }
  auto reply = jobs::webhook::reply_from(status, content_type, body->bytes);
  if (!reply) return fail(Errc::Parse, "\"" + reply.error().content_type + "\" is not a valid MIME type");
  return Delivery{status, std::move(*reply)};
}

}  // namespace campfire::app::webhook
