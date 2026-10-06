// One blocking HTTP/1.1 request over a fresh connection, the way `Net::HTTP` makes it, for the link unfurl: a pinned
// address, TLS that checks the host name, a timeout for the connect and for each read, and a body that is inflated
// when the client asked for it. Rust: crates/campfire/src/integrations/net/http.rs. This is the policy of the unfurl
// only (no proxy, no retries): the other outbound clients keep their own.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/network_guard.hpp"
#include "core/error.hpp"

namespace campfire::app::unfurl {

using Clock = std::chrono::steady_clock;

// What the unfurl uses of the network. The tests replace the parts: fixed DNS answers, a dialer that sends a fake
// public address to a local server, and the CA of a test certificate.
struct Network {
  HostLookup lookup;  // empty: getaddrinfo
  // Changes the address and the port that the client connects to, after the guard checked them.
  std::function<void(std::string& ip, std::uint16_t& port)> dial_override;
  std::string ca_file;  // empty: the default store of OpenSSL
};

struct Endpoint {
  bool https = false;
  std::string host;  // as `URI#host` gives it: the TLS server name
  std::uint16_t port = 80;
  std::string pinned_ip;  // connect here
};

struct Timeouts {
  std::chrono::milliseconds open{5000};
  std::chrono::milliseconds read{5000};
};

struct Request {
  std::string method;  // "GET" or "HEAD"
  std::string target;  // path and query
  std::string host_header;
};

// What reading a body with a size limit produced.
struct Body {
  std::string bytes;
  bool too_large = false;
};

class Connection;

class Response {
 public:
  Response(Response&&) noexcept;
  Response& operator=(Response&&) noexcept;
  ~Response();

  int status = 0;
  std::vector<std::pair<std::string, std::string>> headers;  // names in lower case

  // `response[name]`: every value of the header, joined with ", ".
  [[nodiscard]] std::optional<std::string> header(std::string_view name) const;
  // `Net::HTTPHeader#content_type`: the media type before any ";", each part stripped, not downcased.
  [[nodiscard]] std::optional<std::string> content_type() const;
  // `Net::HTTPHeader#content_length`: the first run of digits. A header without digits is an error.
  [[nodiscard]] Result<std::optional<std::uint64_t>> content_length() const;
  // Reads the body (inflated when `Net::HTTP` would), and stops once it would be longer than `limit` bytes.
  [[nodiscard]] Result<Body> read_body(std::size_t limit);

 private:
  friend Result<Response> exchange(const Network&, const Endpoint&, const Request&, const Timeouts&, Clock::time_point);
  Response() = default;
  std::unique_ptr<Connection> connection_;
  bool decode_content_ = false;
  bool head_ = false;
};

// Connects (within `open`), sends the request and reads the head of the response (each read within `read`). Nothing
// outlasts `deadline`.
[[nodiscard]] Result<Response> exchange(const Network& network, const Endpoint& endpoint, const Request& request,
                                        const Timeouts& timeouts, Clock::time_point deadline);

}  // namespace campfire::app::unfurl
