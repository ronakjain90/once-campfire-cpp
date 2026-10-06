// The front server end to end: the response cache, the compression, the X-Forwarded-* rules, TLS
// with ALPN, the redirect port and HTTP/2. Rust: crates/kit/tests/front.rs, front/tls.rs (tests).
#include <arpa/inet.h>
#include <doctest.h>
#include <netinet/in.h>
#include <nghttp2/nghttp2.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "core/log.hpp"
#include "net/front/acme.hpp"
#include "net/front/tls.hpp"
#include "net/front/tls_keys.hpp"
#include "net/server.hpp"

using namespace campfire;
using namespace campfire::net;
using namespace std::chrono_literals;

namespace {

constexpr const char* kDomain = "chat.example.com";

// One handler for all the tests: the path chooses what the app does (Rust: `test_app`).
Task<Response> test_handler(Ctx& ctx) {
  const Request& request = ctx.request();
  Response response = ctx.response(200);
  if (request.path == "/public") {
    response.add("content-type", "text/plain");
    response.add("cache-control", "public, max-age=60");
    response.add("etag", "\"v1\"");
    // The app's own vary: the entry is then keyed on the encoding (Rust: `test_app`'s /public).
    response.add("vary", "Accept-Encoding");
    response.body_view(ctx.arena().copy("render 0"));
  } else if (request.path == "/private") {
    response.add("content-type", "text/plain");
    response.add("cache-control", "private, max-age=60");
    response.add("set-cookie", "session=1; path=/");
    response.body_view(ctx.arena().copy("private"));
  } else if (request.path == "/page") {
    response.add("content-type", "text/html");
    const std::string text = "<p>campfire</p>";
    response.body_view(ctx.arena().copy(std::string(200 * 1024, ' ').insert(0, text)));
  } else if (request.path == "/headers") {
    std::string text;
    for (const Header& h : request.headers) {
      if (!text.empty()) text += " ";
      text += std::string(h.name) + "=" + std::string(h.value);
    }
    text += " uri=" + std::string(request.target);
    response.add("content-type", "text/plain");
    response.body_view(ctx.arena().copy(text));
  } else {
    std::string text = std::string(request.method_text) + " " + std::string(request.path);
    if (!request.query.empty()) text += "?" + std::string(request.query);
    text += " " + std::string(request.body);
    response.add("content-type", "text/plain");
    response.body_view(ctx.arena().copy(text));
  }
  co_return response;
}

struct Reply {
  int status = 0;
  std::vector<std::string> headers;  // "name: value", in the order of the wire
  std::string body;
  bool closed = false;

  [[nodiscard]] std::string_view get(std::string_view name) const {
    for (const std::string& line : headers) {
      const std::size_t mark = line.find(':');
      if (mark == std::string::npos) continue;
      if (iequals(std::string_view(line).substr(0, mark), name)) {
        std::string_view value(line);
        value.remove_prefix(mark + 1);
        while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        return value;
      }
    }
    return {};
  }
  [[nodiscard]] std::size_t count(std::string_view name) const {
    std::size_t n = 0;
    for (const std::string& line : headers) {
      const std::size_t mark = line.find(':');
      if (mark != std::string::npos && iequals(std::string_view(line).substr(0, mark), name)) ++n;
    }
    return n;
  }
  [[nodiscard]] std::string head() const {
    std::string text;
    for (const std::string& line : headers) text += line + "\n";
    return text;
  }
};

// A plain HTTP client that sends one request and reads one response.
class Client {
 public:
  explicit Client(std::uint16_t port) { connect(port); }
  ~Client() {
    if (fd_ >= 0) ::close(fd_);
  }
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  void send(std::string_view text) const {
    while (!text.empty()) {
      const ssize_t n = ::send(fd_, text.data(), text.size(), MSG_NOSIGNAL);
      REQUIRE(n > 0);
      text.remove_prefix(static_cast<std::size_t>(n));
    }
  }

  Reply read_reply() {
    Reply reply;
    std::string head;
    while (head.find("\r\n\r\n") == std::string::npos) {
      if (!fill(head)) {
        reply.closed = true;
        return reply;
      }
    }
    const std::size_t end = head.find("\r\n\r\n");
    std::istringstream lines(head.substr(0, end));
    std::string line;
    std::getline(lines, line);
    if (line.size() > 12) reply.status = std::stoi(line.substr(9, 3));
    while (std::getline(lines, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      reply.headers.push_back(line);
    }
    if (!reply.get("content-length").empty()) {
      const std::size_t want = std::stoul(std::string(reply.get("content-length")));
      while (head.size() < end + 4 + want) {
        if (!fill(head)) break;
      }
      reply.body = head.substr(end + 4, want);
    }
    return reply;
  }

  [[nodiscard]] int fd() const noexcept { return fd_; }

  // Reads until the peer closes. Returns the bytes.
  std::string read_to_end(int timeout_s = 5) {
    timeval tv{timeout_s, 0};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::string all;
    char chunk[4096];
    while (true) {
      const ssize_t n = ::recv(fd_, chunk, sizeof chunk, 0);
      if (n <= 0) break;
      all.append(chunk, static_cast<std::size_t>(n));
    }
    return all;
  }

 private:
  void connect(std::uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    REQUIRE(::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
  }
  bool fill(std::string& text) {
    char chunk[8192];
    const ssize_t n = ::recv(fd_, chunk, sizeof chunk, 0);
    if (n <= 0) return false;
    text.append(chunk, static_cast<std::size_t>(n));
    return true;
  }
  int fd_ = -1;
};

struct Fixture {
  explicit Fixture(ServerOptions options = {}) {
    options.http_port = 0;
    options.target_port = 0;
    options.https_port = 0;
    options.workers = 2;
    options.serve_static = false;
    server = std::make_unique<Server>(options, App{nullptr, &test_handler});
    REQUIRE(server->start().has_value());
  }
  [[nodiscard]] std::uint16_t http() const { return server->http_port(); }
  [[nodiscard]] std::uint16_t https() const { return server->https_port(); }
  [[nodiscard]] std::uint16_t target() const { return server->target_port(); }
  std::unique_ptr<Server> server;
};

std::string x_cache_of(std::uint16_t port, std::string_view path, std::string_view headers = "") {
  Client c(port);
  c.send("GET " + std::string(path) + " HTTP/1.1\r\nHost: x\r\n" + std::string(headers) + "Connection: close\r\n\r\n");
  const Reply reply = c.read_reply();
  return reply.status == 0 ? std::string() : std::string(reply.get("x-cache"));
}

// --- TLS -----------------------------------------------------------------------------------------

// A storage directory with the cache file of `domain`, the way autocert's DirCache writes it.
std::filesystem::path cached_certificate(const std::filesystem::path& root, std::string_view domain) {
  std::filesystem::create_directories(root);
  const std::time_t now = std::time(nullptr);
  // 90 days: the manager renews 30 days before the end, and a test must not ask a CA.
  const auto certificate = front::self_signed(domain, now - 3600, now + 90 * 86400);
  REQUIRE(certificate.has_value());
  const auto entry = front::cache_entry((*certificate)->key.get(), front::certificate_pem((*certificate)->leaf.get()));
  REQUIRE(entry.has_value());
  const auto written = front::write_cache_file(root, domain, *entry);
  REQUIRE(written.has_value());
  return root;
}

std::shared_ptr<front::TlsServer> tls_server(const std::filesystem::path& storage) {
  FrontConfig config;
  config.tls_domains = {std::string(kDomain)};
  config.storage_path = storage;
  // A test never asks a CA for a certificate: this address has nothing on it.
  config.acme_directory_url = "https://127.0.0.1:1/directory";
  return std::make_shared<front::TlsServer>(
      std::make_shared<front::CertManager>(front::AcmeOptions::from_config(config)));
}

int connect_to(std::uint16_t port);

// The wire format of an ALPN list: the length of each name, then the name.
std::vector<unsigned char> alpn_list(std::string_view name) {
  std::vector<unsigned char> list{static_cast<unsigned char>(name.size())};
  list.insert(list.end(), name.begin(), name.end());
  return list;
}

// A TLS client that trusts the certificate of the test storage directory.
struct ClientTls {
  ClientTls(std::uint16_t port, std::string_view alpn) {
    fd = connect_to(port);
    REQUIRE(fd >= 0);
    SSL_CTX* context = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
    ssl = SSL_new(context);
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, kDomain);
    if (!alpn.empty()) {
      const std::vector<unsigned char> list = alpn_list(alpn);
      SSL_set_alpn_protos(ssl, list.data(), static_cast<unsigned int>(list.size()));
    }
    REQUIRE(SSL_connect(ssl) == 1);
    const unsigned char* data = nullptr;
    unsigned size = 0;
    SSL_get0_alpn_selected(ssl, &data, &size);
    protocol.assign(reinterpret_cast<const char*>(data), size);
    SSL_CTX_free(context);
  }
  ~ClientTls() {
    SSL_free(ssl);
    ::close(fd);
  }
  ClientTls(const ClientTls&) = delete;
  ClientTls& operator=(const ClientTls&) = delete;

  void send(std::string_view text) const {
    while (!text.empty()) {
      const int n = SSL_write(ssl, text.data(), static_cast<int>(text.size()));
      REQUIRE(n > 0);
      text.remove_prefix(static_cast<std::size_t>(n));
    }
  }
  // Reads until the peer closes (the test sends "Connection: close") or the time is over.
  [[nodiscard]] std::string read_all(int timeout_s = 5) const {
    timeval tv{timeout_s, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::string all;
    char chunk[8192];
    while (true) {
      const int n = SSL_read(ssl, chunk, sizeof chunk);
      if (n <= 0) break;
      all.append(chunk, static_cast<std::size_t>(n));
    }
    return all;
  }
  [[nodiscard]] SSL* handle() const noexcept { return ssl; }
  std::string protocol;
  SSL* ssl = nullptr;
  int fd = -1;
};

int connect_to(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

}  // namespace

TEST_CASE("the front caches public responses, variants and ranges") {
  Fixture f;
  CHECK(x_cache_of(f.http(), "/public", "Accept-Encoding: identity\r\n") == "miss");
  CHECK(x_cache_of(f.http(), "/public?", "Accept-Encoding: identity\r\n") == "hit");
  // Another Accept-Encoding is another variant of the same key.
  CHECK(x_cache_of(f.http(), "/public", "Accept-Encoding: br\r\n") == "miss");
  CHECK(x_cache_of(f.http(), "/public", "Accept-Encoding: br\r\n") == "hit");
  // A conditional request gets 304 from the entry.
  {
    CHECK(x_cache_of(f.http(), "/public?c=1", "Accept-Encoding: identity\r\n") == "miss");
    Client c(f.http());
    c.send(
        "GET /public?c=1 HTTP/1.1\r\nHost: x\r\nAccept-Encoding: identity\r\nIf-None-Match: \"v1\"\r\nConnection: "
        "close\r\n\r\n");
    const Reply reply = c.read_reply();
    CHECK(reply.status == 304);
    CHECK(reply.get("x-cache") == "hit");
    CHECK(reply.body.empty());
  }
  // A range goes to the app.
  CHECK(x_cache_of(f.http(), "/public?r=1", "Range: bytes=0-1\r\n") == "bypass");
  // A target over 2 KB is not cacheable.
  const std::string long_path = "/public?pad=" + std::string(4096, 'x');
  CHECK(x_cache_of(f.http(), long_path, "Accept-Encoding: identity\r\n") == "bypass");
  // A private response keeps its cookie and is stored anyway (it is the app that decides).
  {
    Client c(f.http());
    c.send("GET /private?p=1 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    const Reply reply = c.read_reply();
    CHECK(reply.get("x-cache") == "miss");
    CHECK(reply.get("set-cookie") == "session=1; path=/");
    CHECK(reply.count("vary") == 1);
  }
}

TEST_CASE("the front compresses what the app left unencoded") {
  Fixture f;
  {
    Client c(f.http());
    c.send("GET /page?z=1 HTTP/1.1\r\nHost: x\r\nAccept-Encoding: zstd\r\nConnection: close\r\n\r\n");
    const Reply reply = c.read_reply();
    CHECK(reply.get("content-encoding") == "zstd");
    CHECK(reply.body.size() < 1024);
  }
  {
    Client c(f.http());
    c.send("GET /page?g=1 HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\nConnection: close\r\n\r\n");
    const Reply reply = c.read_reply();
    CHECK(reply.get("content-encoding") == "gzip");
  }
  {
    Client c(f.http());
    c.send("GET /page?i=1 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    const Reply reply = c.read_reply();
    CHECK(reply.get("content-encoding").empty());
  }
}

TEST_CASE("the front sets the X-Forwarded-* headers and keeps the client's when it trusts them") {
  {
    Fixture f;
    Client c(f.http());
    c.send(
        "GET /headers HTTP/1.1\r\nHost: chat.test\r\nX-Forwarded-For: 203.0.113.9\r\nForwarded: for=1.2.3.4\r\n"
        "Connection: close\r\n\r\n");
    const Reply reply = c.read_reply();
    CHECK(reply.body.find("forwarded=") == std::string::npos);  // the front drops it
    CHECK(reply.body.find("x-request-start=t=") != std::string::npos);
    // The client's own address comes first, then the address of the connection (FORWARD_HEADERS).
    CHECK(reply.body.find("x-forwarded-for=203.0.113.9, 127.0.0.1") != std::string::npos);
    CHECK(reply.body.find("x-forwarded-proto=http") != std::string::npos);
    CHECK(reply.body.find("x-forwarded-host=chat.test") != std::string::npos);
  }
  {
    // With TLS on, FORWARD_HEADERS is off unless it says otherwise: the client's values are dropped.
    FrontConfig config;
    const auto storage = cached_certificate(std::filesystem::temp_directory_path() / "campfire-a8-forward", kDomain);
    ServerOptions options;
    options.front_headers = true;
    options.tls = tls_server(storage);
    options.serve_static = false;
    config.tls_domains = {std::string(kDomain)};
    options.front = std::make_shared<front::Front>(config);
    Fixture f(options);
    Client c(f.http());
    c.send("GET /headers?fwd=1 HTTP/1.1\r\nHost: " + std::string(kDomain) +
           "\r\nX-Forwarded-For: 203.0.113.9\r\nConnection: close\r\n\r\n");
    const Reply reply = c.read_reply();
    CHECK(reply.status == 301);  // the HTTP port redirects while TLS is on
    CHECK(reply.get("location") == "https://chat.example.com/headers?fwd=1");
    CHECK(reply.get("connection") == "close");
  }
}

TEST_CASE("the HTTP port answers HTTP-01 challenges and redirects when TLS is on") {
  const auto storage = cached_certificate(std::filesystem::temp_directory_path() / "campfire-a8-redirect", kDomain);
  ServerOptions options;
  options.tls = tls_server(storage);
  Fixture f(options);

  Client c(f.http());
  c.send("GET /rooms?x=1 HTTP/1.1\r\nHost: Chat.Example.com:80\r\nConnection: close\r\n\r\n");
  Reply reply = c.read_reply();
  CHECK(reply.status == 301);
  CHECK(reply.get("location") == "https://chat.example.com/rooms?x=1");
  CHECK(reply.get("content-type") == "text/html; charset=utf-8");
  CHECK(reply.body == "<a href=\"https://chat.example.com/rooms?x=1\">Moved Permanently</a>.\n\n");

  Client other(f.http());
  other.send("GET / HTTP/1.1\r\nHost: evil.example.com\r\nConnection: close\r\n\r\n");
  reply = other.read_reply();
  CHECK(reply.status == 421);
  CHECK(reply.body == "Misdirected Request\n");

  // A POST gets no content type and no body.
  Client post(f.http());
  post.send("POST /session HTTP/1.1\r\nHost: chat.example.com\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  reply = post.read_reply();
  CHECK(reply.status == 301);
  CHECK(reply.get("content-type").empty());
  CHECK(reply.body.empty());

  // HTTP-01: a token that is not in the cache is a 404, a host that is not a TLS_DOMAIN is a 403.
  Client challenge(f.http());
  challenge.send("GET /.well-known/acme-challenge/abc HTTP/1.1\r\nHost: chat.example.com\r\nConnection: close\r\n\r\n");
  reply = challenge.read_reply();
  CHECK(reply.status == 404);
  CHECK(reply.body == "acme/autocert: certificate cache miss\n");
  Client refused(f.http());
  refused.send("GET /.well-known/acme-challenge/abc HTTP/1.1\r\nHost: other.example.com\r\nConnection: close\r\n\r\n");
  CHECK(refused.read_reply().status == 403);

  // A token that an order set is the body of a 200.
  options.tls->certs().set_http_token("/.well-known/acme-challenge/abc", "abc.thumbprint");
  Client answered(f.http());
  answered.send("GET /.well-known/acme-challenge/abc HTTP/1.1\r\nHost: chat.example.com\r\nConnection: close\r\n\r\n");
  reply = answered.read_reply();
  CHECK(reply.status == 200);
  CHECK(reply.get("content-type") == "text/plain; charset=utf-8");
  CHECK(reply.body == "abc.thumbprint");
}

// The server logs what it does with a handshake, so the TLS tests can show it.
struct LogDebug {
  LogDebug() { Logger::instance().set_level(LogLevel::Debug); }
  ~LogDebug() { Logger::instance().set_level(LogLevel::Info); }
};

TEST_CASE("the HTTPS port serves HTTP/1.1 over TLS") {
  const LogDebug debug;
  const auto storage = cached_certificate(std::filesystem::temp_directory_path() / "campfire-a8-tls", kDomain);
  ServerOptions options;
  options.tls = tls_server(storage);
  Fixture f(options);
  ClientTls client(f.https(), "http/1.1");
  CHECK(client.protocol == "http/1.1");
  client.send("GET /tls1 HTTP/1.1\r\nHost: " + std::string(kDomain) + "\r\nConnection: close\r\n\r\n");
  const std::string raw = client.read_all();
  CHECK(raw.rfind("HTTP/1.1 200", 0) == 0);
  CHECK(raw.find("GET /tls1") != std::string::npos);
  // The front adds the response headers (x-cache, vary), and X-Forwarded-Proto to the request.
  CHECK(raw.find("x-cache: miss") != std::string::npos);
  ClientTls echo(f.https(), "http/1.1");
  echo.send("GET /headers HTTP/1.1\r\nHost: " + std::string(kDomain) + "\r\nConnection: close\r\n\r\n");
  CHECK(echo.read_all().find("x-forwarded-proto=https") != std::string::npos);
}

TEST_CASE("a client with no ALPN gets HTTP/1.1") {
  const auto storage = cached_certificate(std::filesystem::temp_directory_path() / "campfire-a8-alpn", kDomain);
  ServerOptions options;
  options.tls = tls_server(storage);
  Fixture f(options);
  ClientTls client(f.https(), "");
  CHECK(client.protocol.empty());
  client.send("GET /noalpn HTTP/1.1\r\nHost: " + std::string(kDomain) + "\r\nConnection: close\r\n\r\n");
  CHECK(client.read_all().rfind("HTTP/1.1 200", 0) == 0);
}

TEST_CASE("the handshake fails for a host that is not a TLS_DOMAIN") {
  const auto storage = cached_certificate(std::filesystem::temp_directory_path() / "campfire-a8-sni", kDomain);
  ServerOptions options;
  options.tls = tls_server(storage);
  Fixture f(options);
  const int fd = connect_to(f.https());
  REQUIRE(fd >= 0);
  SSL_CTX* context = SSL_CTX_new(TLS_client_method());
  SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
  SSL* ssl = SSL_new(context);
  SSL_set_fd(ssl, fd);
  SSL_set_tlsext_host_name(ssl, "evil.example.com");
  CHECK(SSL_connect(ssl) != 1);
  SSL_free(ssl);
  SSL_CTX_free(context);
  ::close(fd);
}
