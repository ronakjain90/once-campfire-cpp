// TLS for the HTTPS port. Rust: crates/kit/src/front/tls.rs.
#include "net/front/tls.hpp"

#include <openssl/err.h>

#include <cstring>

#include "core/log.hpp"

namespace campfire::net::front {

namespace {

constexpr std::string_view kAcmeAlpn = "acme-tls/1";

// The tag of a connection in the data of the `SSL` object: set by the ClientHello callback.
int challenge_index() {
  static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
  return index;
}

struct Hello {
  std::string server_name;
  bool acme = false;
};

Hello parse_hello(SSL* ssl) {
  Hello hello;
  const unsigned char* data = nullptr;
  std::size_t size = 0;
  // server_name: u16 list length, then (u8 type, u16 length, name).
  if (SSL_client_hello_get0_ext(ssl, TLSEXT_TYPE_server_name, &data, &size) == 1 && size >= 5) {
    const std::size_t name_length = (static_cast<std::size_t>(data[3]) << 8) | data[4];
    if (data[2] == 0 && 5 + name_length <= size)
      hello.server_name.assign(reinterpret_cast<const char*>(data) + 5, name_length);
  }
  // ALPN: u16 list length, then (u8 length, protocol).
  if (SSL_client_hello_get0_ext(ssl, TLSEXT_TYPE_application_layer_protocol_negotiation, &data, &size) == 1 &&
      size >= 2) {
    std::size_t at = 2;
    while (at < size) {
      const std::size_t length = data[at++];
      if (at + length > size) break;
      if (std::string_view(reinterpret_cast<const char*>(data) + at, length) == kAcmeAlpn) hello.acme = true;
      at += length;
    }
  }
  return hello;
}

bool use_certificate(SSL* ssl, const CertifiedKey& certificate) {
  if (SSL_use_certificate(ssl, certificate.leaf.get()) != 1) return false;
  if (SSL_use_PrivateKey(ssl, certificate.key.get()) != 1) return false;
  if (!certificate.chain.empty()) {
    STACK_OF(X509)* chain = sk_X509_new_null();
    for (const X509Ptr& cert : certificate.chain) {
      X509_up_ref(cert.get());
      sk_X509_push(chain, cert.get());
    }
    SSL_set0_chain(ssl, chain);
  }
  return true;
}

int client_hello_callback(SSL* ssl, int* alert, void* argument) {
  auto* server = static_cast<TlsServer*>(argument);
  const Hello hello = parse_hello(ssl);
  if (hello.acme) {
    SSL_set_ex_data(ssl, challenge_index(), reinterpret_cast<void*>(1));
    const auto certificate = server->certs().challenge_certificate_for(hello.server_name);
    if (!certificate || !use_certificate(ssl, *certificate)) {
      *alert = SSL_AD_UNRECOGNIZED_NAME;
      return SSL_CLIENT_HELLO_ERROR;
    }
    return SSL_CLIENT_HELLO_SUCCESS;
  }
  std::shared_ptr<const CertifiedKey> certificate;
  std::string error;
  switch (server->certs().certificate(hello.server_name, certificate, error)) {
    case CertManager::State::Ready:
      if (!use_certificate(ssl, *certificate)) {
        *alert = SSL_AD_INTERNAL_ERROR;
        return SSL_CLIENT_HELLO_ERROR;
      }
      return SSL_CLIENT_HELLO_SUCCESS;
    case CertManager::State::Pending: return SSL_CLIENT_HELLO_RETRY;
    case CertManager::State::Failed: break;
  }
  log_debug("http: TLS handshake error server_name={} error={}", hello.server_name, error);
  *alert = SSL_AD_UNRECOGNIZED_NAME;
  return SSL_CLIENT_HELLO_ERROR;
}

int alpn_callback(SSL* ssl, const unsigned char** out, unsigned char* out_length, const unsigned char* in,
                  unsigned in_length, void* /*argument*/) {
  static constexpr unsigned char kChallenge[] = {10, 'a', 'c', 'm', 'e', '-', 't', 'l', 's', '/', '1'};
  static constexpr unsigned char kServer[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
  const bool challenge = SSL_get_ex_data(ssl, challenge_index()) != nullptr;
  const unsigned char* list = challenge ? kChallenge : kServer;
  const unsigned list_length = challenge ? sizeof kChallenge : sizeof kServer;
  unsigned char* selected = nullptr;
  if (SSL_select_next_proto(&selected, out_length, list, list_length, in, in_length) != OPENSSL_NPN_NEGOTIATED) {
    return SSL_TLSEXT_ERR_ALERT_FATAL;
  }
  *out = selected;
  return SSL_TLSEXT_ERR_OK;
}

}  // namespace

TlsServer::TlsServer(std::shared_ptr<CertManager> certs) : certs_(std::move(certs)) {
  ctx_ = SSL_CTX_new(TLS_server_method());
  SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
  SSL_CTX_set_options(ctx_, SSL_OP_NO_RENEGOTIATION | SSL_OP_CIPHER_SERVER_PREFERENCE);
  SSL_CTX_set_mode(ctx_,
                   SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_RELEASE_BUFFERS);
  SSL_CTX_set_client_hello_cb(ctx_, client_hello_callback, this);
  SSL_CTX_set_alpn_select_cb(ctx_, alpn_callback, nullptr);
  // Session tickets are on (the default), as Go's crypto/tls has them.
  SSL_CTX_set_session_id_context(ctx_, reinterpret_cast<const unsigned char*>("campfire"), 8);
}

TlsServer::~TlsServer() {
  SSL_CTX_free(ctx_);
}

SslPtr TlsServer::accept(int fd) const {
  SslPtr ssl(SSL_new(ctx_));
  if (!ssl) return nullptr;
  SSL_set_fd(ssl.get(), fd);
  SSL_set_accept_state(ssl.get());
  return ssl;
}

bool TlsServer::is_challenge(const SSL* ssl) noexcept {
  return SSL_get_ex_data(ssl, challenge_index()) != nullptr;
}

std::string_view TlsServer::protocol(const SSL* ssl) noexcept {
  const unsigned char* data = nullptr;
  unsigned size = 0;
  SSL_get0_alpn_selected(ssl, &data, &size);
  return {reinterpret_cast<const char*>(data), size};
}

namespace {

std::string html_escape(std::string_view text) {
  std::string out;
  for (const char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&#34;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

// `net.SplitHostPort`, or the host as given.
std::string_view split_host_port(std::string_view host) {
  if (host.starts_with('[')) {
    const std::size_t close = host.find("]:");
    return close == std::string_view::npos ? host : host.substr(1, close - 1);
  }
  const std::size_t colon = host.rfind(':');
  if (colon != std::string_view::npos && host.substr(0, colon).find(':') == std::string_view::npos)
    return host.substr(0, colon);
  return host;
}

std::string_view arena_copy(std::pmr::memory_resource* resource, const std::string& text) {
  char* data = static_cast<char*>(resource->allocate(text.size() + 1, 1));
  std::memcpy(data, text.data(), text.size());
  return {data, text.size()};
}

// `http.Error`
Response text_error(std::pmr::memory_resource* resource, int status, std::string_view message) {
  Response response(resource, status);
  response.add("content-type", "text/plain; charset=utf-8");
  response.add("x-content-type-options", "nosniff");
  response.body_view(arena_copy(resource, std::string(message) + "\n"));
  return response;
}

}  // namespace

Response http_port_response(CertManager& certs, const Request& request, std::pmr::memory_resource* resource) {
  const std::string_view host = request.header("host");
  const std::string_view path = request.path;
  if (path.starts_with("/.well-known/acme-challenge/")) {
    if (!certs.host_allowed(host)) {
      return text_error(resource, 403,
                        "acme/autocert: host \"" + std::string(host) + "\" not configured in HostWhitelist");
    }
    const auto token = certs.http_token(path);
    if (!token) return text_error(resource, 404, "acme/autocert: certificate cache miss");
    Response response(resource, 200);
    response.add("content-type", "text/plain; charset=utf-8");
    response.body_view(arena_copy(resource, *token));
    return response;
  }
  // `httpRedirectHandler`: 301 to the same path over HTTPS for a TLS_DOMAIN host, 421 for others.
  const std::string name = normalize_domain(split_host_port(host));
  const auto build = [&]() {
    if (name.empty() || !certs.host_allowed(name)) return text_error(resource, 421, "Misdirected Request");
    const std::string url = "https://" + name + std::string(request.target);
    Response redirect(resource, 301);
    redirect.add("location", arena_copy(resource, url));
    if (request.method == Method::Get || request.method == Method::Head) {
      redirect.add("content-type", "text/html; charset=utf-8");
      if (request.method == Method::Get) {
        redirect.body_view(arena_copy(resource, "<a href=\"" + html_escape(url) + "\">Moved Permanently</a>.\n\n"));
      }
    }
    return redirect;
  };
  Response response = build();
  response.add("connection", "close");
  return response;
}

}  // namespace campfire::net::front
