// ACME end to end against a local Pebble (TLS-ALPN-01). Rust: crates/kit/tests/front.rs
// (`acme_tls_alpn_certificate_cached_and_reused`). The tests need Pebble: set PEBBLE_MINICA to the
// PEM file of the Pebble CA (test/certs/pebble.minica.pem), and make "campfire.test" resolve to
// 127.0.0.1 for Pebble. Without PEBBLE_MINICA the test returns at once.
#include "net/front/acme.hpp"

#include <arpa/inet.h>
#include <doctest.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include "net/front/tls.hpp"
#include "net/server.hpp"

using namespace campfire;
using namespace campfire::net;

namespace {

constexpr const char* kDomain = "campfire.test";
constexpr std::uint16_t kHttpsPort = 5001;  // Pebble validates TLS-ALPN-01 here

Task<Response> test_handler(Ctx& ctx) {
  Response response = ctx.response(200);
  response.add("content-type", "text/plain");
  response.body_view(ctx.arena().copy("ok"));
  co_return response;
}

const char* pebble_root() {
  return std::getenv("PEBBLE_MINICA");
}

// Pebble validates HTTP-01 on this port. A test for HTTP-01 against Pebble is not possible: Pebble
// sends "Host: campfire.test:5002", autocert (so Thruster and this server) checks the Host header
// against TLS_DOMAIN with its port, and answers 403. HTTP-01 has unit tests in front_test.cpp.
constexpr std::uint16_t kHttpPortForPebble = 5002;
std::uint16_t http_port() {
  return kHttpPortForPebble;
}

front::AcmeOptions acme_options(const std::filesystem::path& storage, const std::string& root,
                                std::vector<front::ChallengeType> types) {
  front::AcmeOptions options;
  options.directory_url = "https://localhost:14000/dir";
  options.directory_root = root;
  options.storage_path = storage;
  options.domains = {kDomain};
  options.challenge_types = std::move(types);
  return options;
}

std::unique_ptr<Server> start(const front::AcmeOptions& acme) {
  ServerOptions options;
  options.http_port = http_port();
  options.https_port = kHttpsPort;
  options.target_port = 0;
  options.listen_target = false;
  options.workers = 2;
  options.serve_static = false;
  options.tls = std::make_shared<front::TlsServer>(std::make_shared<front::CertManager>(acme));
  auto server = std::make_unique<Server>(options, App{nullptr, &test_handler});
  REQUIRE(server->start().has_value());
  return server;
}

struct Handshake {
  std::string alpn;
  std::string issuer;
  bool ok = false;
};

// A TLS handshake with SNI `kDomain`: the negotiated protocol and the issuer of the leaf.
Handshake handshake(std::uint16_t port) {
  Handshake result;
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
  SSL_CTX* context = SSL_CTX_new(TLS_client_method());
  SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
  SSL* ssl = SSL_new(context);
  SSL_set_fd(ssl, fd);
  SSL_set_tlsext_host_name(ssl, kDomain);
  static const unsigned char kAlpn[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
  SSL_set_alpn_protos(ssl, kAlpn, sizeof kAlpn);
  if (SSL_connect(ssl) == 1) {
    result.ok = true;
    const unsigned char* data = nullptr;
    unsigned size = 0;
    SSL_get0_alpn_selected(ssl, &data, &size);
    result.alpn.assign(reinterpret_cast<const char*>(data), size);
    X509* leaf = SSL_get1_peer_certificate(ssl);
    if (leaf != nullptr) {
      char name[512];
      X509_NAME_oneline(X509_get_issuer_name(leaf), name, sizeof name);
      result.issuer = name;
      X509_free(leaf);
    }
  }
  SSL_free(ssl);
  SSL_CTX_free(context);
  ::close(fd);
  return result;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// One plain HTTP request on the HTTP port: the whole reply as text.
std::string http_exchange(std::string_view request) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(http_port());
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
  timeval tv{3, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  [[maybe_unused]] const ssize_t sent = ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
  std::string all;
  char chunk[4096];
  ssize_t n = 0;
  while ((n = ::recv(fd, chunk, sizeof chunk, 0)) > 0) all.append(chunk, static_cast<std::size_t>(n));
  ::close(fd);
  return all;
}

// The first handshake starts the order. The server answers when the order is done: ask again.
Handshake wait_for_certificate() {
  Handshake shake;
  for (int attempt = 0; attempt < 60 && !shake.ok; ++attempt) {
    shake = handshake(kHttpsPort);
    if (!shake.ok) std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  return shake;
}

void check_issued(const std::filesystem::path& storage, const Handshake& shake) {
  CHECK(shake.ok);
  CHECK(shake.alpn == "h2");
  CHECK(shake.issuer.find("Pebble") != std::string::npos);
  const std::string cached = read_file(storage / kDomain);
  CHECK(cached.rfind("-----BEGIN EC PRIVATE KEY-----", 0) == 0);
  CHECK(cached.find("-----BEGIN CERTIFICATE-----") != std::string::npos);
  CHECK(read_file(storage / "acme_account+key").rfind("-----BEGIN EC PRIVATE KEY-----", 0) == 0);
}

}  // namespace

TEST_CASE("ACME TLS-ALPN-01: Pebble issues a certificate, the cache keeps it, a restart reuses it") {
  const char* root = pebble_root();
  if (root == nullptr) return;  // skipped: PEBBLE_MINICA is not set
  const auto storage = std::filesystem::temp_directory_path() / "campfire-a8-pebble-alpn";
  std::filesystem::remove_all(storage);
  std::filesystem::create_directories(storage);
  std::string issuer;
  {
    auto server = start(acme_options(storage, root, {front::ChallengeType::TlsAlpn01}));
    const Handshake shake = wait_for_certificate();
    check_issued(storage, shake);
    issuer = shake.issuer;
    const std::string redirect =
        http_exchange("GET /rooms?x=1 HTTP/1.1\r\nHost: campfire.test\r\nConnection: close\r\n\r\n");
    CHECK(redirect.rfind("HTTP/1.1 301", 0) == 0);
    CHECK(redirect.find("location: https://campfire.test/rooms?x=1") != std::string::npos);
    const std::string misdirected = http_exchange("GET / HTTP/1.1\r\nHost: other.test\r\nConnection: close\r\n\r\n");
    CHECK(misdirected.rfind("HTTP/1.1 421", 0) == 0);
  }
  // A restart serves the cached certificate, and it does not ask the CA (the directory is gone).
  front::AcmeOptions offline = acme_options(storage, root, {front::ChallengeType::TlsAlpn01});
  offline.directory_url = "https://127.0.0.1:9/dir";
  auto server = start(offline);
  const Handshake again = handshake(kHttpsPort);
  CHECK(again.ok);
  CHECK(again.issuer == issuer);
}
