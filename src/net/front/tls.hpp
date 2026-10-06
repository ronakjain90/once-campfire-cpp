// TLS for the HTTPS port: OpenSSL contexts, SNI, ALPN, ACME challenge handshakes, and the answers of the
// HTTP port when TLS is on. Rust: crates/kit/src/front/tls.rs.
#pragma once

#include <openssl/ssl.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "net/front/acme.hpp"
#include "net/response.hpp"

namespace campfire::net::front {

struct SslDeleter {
  void operator()(SSL* ssl) const noexcept { SSL_free(ssl); }
};
using SslPtr = std::unique_ptr<SSL, SslDeleter>;

class TlsServer {
 public:
  explicit TlsServer(std::shared_ptr<CertManager> certs);
  ~TlsServer();
  TlsServer(const TlsServer&) = delete;
  TlsServer& operator=(const TlsServer&) = delete;

  // A new connection state for a socket. The handshake runs with `SSL_accept`.
  [[nodiscard]] SslPtr accept(int fd) const;
  // True if the handshake of this connection is a TLS-ALPN-01 validation: close the connection after it.
  [[nodiscard]] static bool is_challenge(const SSL* ssl) noexcept;
  // The protocol that ALPN chose: "h2", "http/1.1", or "" if the client sent none.
  [[nodiscard]] static std::string_view protocol(const SSL* ssl) noexcept;

  [[nodiscard]] CertManager& certs() const noexcept { return *certs_; }

 private:
  std::shared_ptr<CertManager> certs_;
  SSL_CTX* ctx_ = nullptr;
};

// What the HTTP port answers when TLS is on (`manager.HTTPHandler(httpRedirectHandler)`): the
// HTTP-01 answers, and a permanent redirect to HTTPS for everything else.
[[nodiscard]] Response http_port_response(CertManager& certs, const Request& request,
                                          std::pmr::memory_resource* resource);

}  // namespace campfire::net::front
