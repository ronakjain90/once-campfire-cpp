// HTTP/2: streams over TLS and over cleartext (H2C_ENABLED). Rust: crates/kit/tests/front.rs
// (`speaks_h2c_only_when_enabled`, `http2_connections_close_after_the_idle_timeout`).
#include <arpa/inet.h>
#include <doctest.h>
#include <netinet/in.h>
#include <nghttp2/nghttp2.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
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

Task<Response> test_handler(Ctx& ctx) {
  const Request& request = ctx.request();
  Response response = ctx.response(200);
  std::string text = std::string(request.method_text) + " " + std::string(request.path);
  if (!request.query.empty()) text += "?" + std::string(request.query);
  text += " " + std::string(request.body);
  for (const Header& h : request.headers) {
    text += "\n" + std::string(h.name) + ": " + std::string(h.value);
  }
  response.add("content-type", "text/plain");
  response.add("set-cookie", "a=1");
  response.add_copy("content-length", std::to_string(text.size() + 20));
  response.body_view(ctx.arena().copy(text));
  co_return response;
}

struct Fixture {
  explicit Fixture(ServerOptions options = {}) {
    options.http_port = 0;
    options.target_port = 0;
    options.https_port = 0;
    options.listen_target = false;
    options.workers = 2;
    options.serve_static = false;
    server = std::make_unique<Server>(options, App{nullptr, &test_handler});
    REQUIRE(server->start().has_value());
  }
  [[nodiscard]] std::uint16_t http() const { return server->http_port(); }
  [[nodiscard]] std::uint16_t https() const { return server->https_port(); }
  std::unique_ptr<Server> server;
};

// One HTTP/2 response, as the client sees it.
struct H2Reply {
  int status = 0;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  bool ended = false;

  [[nodiscard]] std::string_view get(std::string_view name) const {
    for (const auto& [field, value] : headers) {
      if (iequals(field, name)) return value;
    }
    return {};
  }
};

// The nghttp2 client needs the header fields and the data of a frame in its own callbacks. The
// struct is the `user_data` of the client session.
struct ClientState {
  H2Reply reply;
  std::string pending;
  bool ended = false;
};

int on_header(nghttp2_session*, const nghttp2_frame*, const std::uint8_t* name, std::size_t name_len,
              const std::uint8_t* value, std::size_t value_len, std::uint8_t, void* data) {
  auto* state = static_cast<ClientState*>(data);
  const std::string field(reinterpret_cast<const char*>(name), name_len);
  const std::string text(reinterpret_cast<const char*>(value), value_len);
  if (field == ":status") state->reply.status = std::stoi(text);
  state->reply.headers.emplace_back(field, text);
  return 0;
}

int on_data(nghttp2_session*, std::uint8_t, std::int32_t, const std::uint8_t* data, std::size_t length,
            void* user_data) {
  static_cast<ClientState*>(user_data)->reply.body.append(reinterpret_cast<const char*>(data), length);
  return 0;
}

int on_frame_recv(nghttp2_session*, const nghttp2_frame* frame, void* data) {
  auto* state = static_cast<ClientState*>(data);
  if (frame->hd.type == NGHTTP2_HEADERS && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
    state->ended = true;
    state->reply.ended = true;
  }
  if (frame->hd.type == NGHTTP2_DATA && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
    state->ended = true;
    state->reply.ended = true;
  }
  return 0;
}

// An HTTP/2 client over one socket, plain or TLS.
class H2Client {
 public:
  H2Client(std::uint16_t port, bool tls) {
    fd_ = connect_to(port);
    REQUIRE(fd_ >= 0);
    if (tls) {
      context_ = SSL_CTX_new(TLS_client_method());
      SSL_CTX_set_verify(context_, SSL_VERIFY_NONE, nullptr);
      ssl_ = SSL_new(context_);
      SSL_set_fd(ssl_, fd_);
      SSL_set_tlsext_host_name(ssl_, kDomain);
      static const unsigned char kAlpn[] = {2, 'h', '2'};
      SSL_set_alpn_protos(ssl_, kAlpn, sizeof kAlpn);
      REQUIRE(SSL_connect(ssl_) == 1);
      const unsigned char* data = nullptr;
      unsigned size = 0;
      SSL_get0_alpn_selected(ssl_, &data, &size);
      alpn_.assign(reinterpret_cast<const char*>(data), size);
    } else {
      // H2C_ENABLED: the client speaks HTTP/2 with prior knowledge. nghttp2 sends the client
      // connection preface itself on the first `drain()`, so the server's first bytes are it.
    }
    nghttp2_session_callbacks* callbacks = nullptr;
    REQUIRE(nghttp2_session_callbacks_new(&callbacks) == 0);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv);
    callbacks_ = callbacks;
    nghttp2_session* session = nullptr;
    REQUIRE(nghttp2_session_client_new(&session, callbacks_, &state_) == 0);
    session_ = session;
    // A client sends its SETTINGS right after the connection preface (RFC 9113 section 3.4).
    const nghttp2_settings_entry settings[] = {{NGHTTP2_SETTINGS_ENABLE_PUSH, 0}};
    [[maybe_unused]] const int sent = nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, settings, 1);
    REQUIRE(sent == 0);
    drain();
  }
  ~H2Client() {
    if (session_ != nullptr) nghttp2_session_del(session_);
    if (callbacks_ != nullptr) nghttp2_session_callbacks_del(callbacks_);
    if (ssl_ != nullptr) {
      SSL_free(ssl_);
      SSL_CTX_free(context_);
    }
    if (fd_ >= 0) ::close(fd_);
  }
  H2Client(const H2Client&) = delete;
  H2Client& operator=(const H2Client&) = delete;

  // Sends one request. `body` is the DATA of the request.
  void request(std::string_view path, std::string_view body, std::uint16_t port) {
    const std::string method = body.empty() ? "GET" : "POST";
    const std::string scheme = port != 0 ? "https" : "http";
    const std::string authority = port != 0 ? std::string(kDomain) + ":" + std::to_string(port) : std::string(kDomain);
    std::string_view names[] = {":method", ":path", ":scheme", ":authority"};
    std::string values[] = {method, std::string(path), scheme, authority};
    std::vector<nghttp2_nv> fields;
    for (std::size_t i = 0; i < 4; ++i) {
      fields.push_back({reinterpret_cast<std::uint8_t*>(const_cast<char*>(names[i].data())),
                        reinterpret_cast<std::uint8_t*>(values[i].data()), names[i].size(), values[i].size(),
                        NGHTTP2_NV_FLAG_NONE});
    }
    bodies_.push_back(std::string(body));
    nghttp2_data_provider provider{};
    provider.source.ptr = &bodies_.back();
    provider.read_callback = [](nghttp2_session*, std::int32_t, std::uint8_t* data, std::size_t length,
                                std::uint32_t*, nghttp2_data_source* source, void*) -> ssize_t {
      const auto* text = static_cast<const std::string*>(source->ptr);
      const std::size_t take = std::min(text->size(), length);
      std::memcpy(data, text->data(), take);
      return static_cast<ssize_t>(take);
    };
    // A request body goes with the HEADERS of the request, as one data provider (not as a
    // separate `nghttp2_submit_data`: the HEADERS already ended the stream for a GET).
    REQUIRE(nghttp2_submit_request(session_, nullptr, fields.data(), fields.size(), body.empty() ? nullptr : &provider,
                                   nullptr) >= 0);
    drain();
  }

  // Reads until a response ends. Returns false if the connection closed first.
  bool receive(H2Reply& out, int timeout_s = 5) {
    ClientState& state = state_;
    state = ClientState{};
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(timeout_s)) {
      timeval tv{1, 0};
      setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
      char chunk[16384];
      const int n =
          ssl_ != nullptr ? SSL_read(ssl_, chunk, sizeof chunk) : static_cast<int>(::recv(fd_, chunk, sizeof chunk, 0));
      if (n <= 0) {
        if (state.ended) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
        break;
      }
      state.pending.append(chunk, static_cast<std::size_t>(n));
      while (!state.pending.empty()) {
        const ssize_t used = nghttp2_session_mem_recv(
            session_, reinterpret_cast<const std::uint8_t*>(state.pending.data()), state.pending.size());
        if (used < 0) return false;
        state.pending.erase(0, static_cast<std::size_t>(used));
      }
      drain();
      if (state.ended) break;
    }
    out = state.reply;
    return state.ended;
  }

  // True when the peer closed the connection (at `timeout_s` at the latest).
  bool wait_for_close(int timeout_s) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(timeout_s)) {
      timeval tv{1, 0};
      setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
      char chunk[4096];
      const int n =
          ssl_ != nullptr ? SSL_read(ssl_, chunk, sizeof chunk) : static_cast<int>(::recv(fd_, chunk, sizeof chunk, 0));
      if (n <= 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] const std::string& alpn() const noexcept { return alpn_; }

 private:
  static int connect_to(std::uint16_t port) {
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
  void send_bytes(const std::uint8_t* data, std::size_t length) {
    std::size_t at = 0;
    while (at < length) {
      const int n = ssl_ != nullptr ? SSL_write(ssl_, data + at, static_cast<int>(length - at))
                                    : static_cast<int>(::send(fd_, data + at, length - at, MSG_NOSIGNAL));
      if (n <= 0) return;
      at += static_cast<std::size_t>(n);
    }
  }
  void drain() {
    while (true) {
      const std::uint8_t* frame = nullptr;
      const ssize_t n = nghttp2_session_mem_send2(session_, &frame);
      if (n <= 0) break;
      send_bytes(frame, static_cast<std::size_t>(n));
    }
  }
  int fd_ = -1;
  SSL_CTX* context_ = nullptr;
  SSL* ssl_ = nullptr;
  nghttp2_session* session_ = nullptr;
  nghttp2_session_callbacks* callbacks_ = nullptr;
  std::string alpn_;
  ClientState state_;               // the `user_data` of `session_`
  std::deque<std::string> bodies_;  // the DATA of the requests: `std::deque` keeps the pointers
};

std::shared_ptr<front::TlsServer> tls_server(const std::filesystem::path& storage) {
  FrontConfig config;
  config.tls_domains = {std::string(kDomain)};
  config.storage_path = storage;
  // A test never asks a CA for a certificate: this address has nothing on it.
  config.acme_directory_url = "https://127.0.0.1:1/directory";
  return std::make_shared<front::TlsServer>(
      std::make_shared<front::CertManager>(front::AcmeOptions::from_config(config)));
}

std::filesystem::path cached_certificate(const std::filesystem::path& root) {
  std::filesystem::create_directories(root);
  const std::time_t now = std::time(nullptr);
  // 90 days: the manager renews 30 days before the end, and a test must not ask a CA.
  const auto certificate = front::self_signed(kDomain, now - 3600, now + 90 * 86400);
  REQUIRE(certificate.has_value());
  // The cache file of autocert: the private key, then the chain.
  const auto entry = front::cache_entry((*certificate)->key.get(), front::certificate_pem((*certificate)->leaf.get()));
  REQUIRE(entry.has_value());
  REQUIRE(front::write_cache_file(root, kDomain, *entry).has_value());
  return root;
}

}  // namespace

TEST_CASE("the HTTPS port speaks HTTP/2 when ALPN chooses it") {
  const auto storage = cached_certificate(std::filesystem::temp_directory_path() / "campfire-a8-h2tls");
  ServerOptions options;
  options.tls = tls_server(storage);
  Fixture f(options);
  H2Client client(f.https(), true);
  client.request("/h2?a=1", {}, f.https());
  H2Reply reply;
  REQUIRE(client.receive(reply));
  CHECK(reply.status == 200);
  CHECK(reply.ended);
  CHECK(reply.body.rfind("GET /h2?a=1", 0) == 0);
  // The header names of HTTP/2 are lowercase, and the front's headers are there.
  CHECK(reply.get("content-type") == "text/plain");
  CHECK(reply.get("x-cache") == "miss");
  CHECK(reply.get("vary") == "Accept-Encoding");
  CHECK_FALSE(reply.get("date").empty());
  CHECK(reply.get("x-forwarded-proto") == "https");
  // HTTP/2 forbids connection-specific headers.
  CHECK(reply.get("connection").empty());
  CHECK(reply.get("transfer-encoding").empty());
  CHECK(reply.get("content-length") == std::to_string(reply.body.size()));
  // The app sees the request as an HTTP/1.1 one with a host and an origin-form path.
  CHECK(reply.body.find("\nhost: chat.example.com:") != std::string::npos);
}

TEST_CASE("an HTTP/2 request with a body reaches the handler") {
  const auto storage = cached_certificate(std::filesystem::temp_directory_path() / "campfire-a8-h2body");
  ServerOptions options;
  options.tls = tls_server(storage);
  Fixture f(options);
  H2Client client(f.https(), true);
  client.request("/upload", "hello h2", f.https());
  H2Reply reply;
  REQUIRE(client.receive(reply));
  CHECK(reply.status == 200);
  CHECK(reply.body.rfind("POST /upload hello h2", 0) == 0);
}

TEST_CASE("several streams on one connection, and a second request after the first") {
  const auto storage = cached_certificate(std::filesystem::temp_directory_path() / "campfire-a8-h2many");
  ServerOptions options;
  options.tls = tls_server(storage);
  Fixture f(options);
  H2Client client(f.https(), true);
  client.request("/one", {}, f.https());
  H2Reply first;
  REQUIRE(client.receive(first));
  CHECK(first.status == 200);
  client.request("/two", {}, f.https());
  H2Reply second;
  REQUIRE(client.receive(second));
  CHECK(second.status == 200);
  CHECK(second.body.rfind("GET /two", 0) == 0);
}

TEST_CASE("a cleartext client speaks HTTP/2 only when H2C_ENABLED is on") {
  Logger::instance().set_level(LogLevel::Debug);
  {
    ServerOptions options;
    options.h2c = true;
    Fixture f(options);
    H2Client client(f.http(), false);
    client.request("/private", {}, 0);
    H2Reply reply;
    REQUIRE(client.receive(reply));
    CHECK(reply.status == 200);
    CHECK(reply.body.rfind("GET /private", 0) == 0);
    CHECK(reply.get("x-cache") == "miss");
    CHECK(reply.body.find("\nhost: chat.example.com") != std::string::npos);
  }
  {
    // Without H2C_ENABLED the same bytes are an HTTP/1.1 request that no route matches.
    Fixture f;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(f.http());
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
    const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    [[maybe_unused]] const ssize_t n = ::send(fd, preface.data(), preface.size(), MSG_NOSIGNAL);
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char chunk[4096];
    const ssize_t got = ::recv(fd, chunk, sizeof chunk, 0);
    CHECK(got <= 0);  // the server gives up on a head it cannot parse
    ::close(fd);
  }
}

TEST_CASE("an HTTP/2 connection closes after the idle timeout") {
  ServerOptions options;
  options.h2c = true;
  options.idle_timeout_ms = 2000;
  options.read_timeout_ms = 1000;
  Fixture f(options);
  H2Client client(f.http(), false);
  client.request("/idle", {}, 0);
  H2Reply reply;
  REQUIRE(client.receive(reply));
  CHECK(reply.status == 200);
  const auto start = std::chrono::steady_clock::now();
  CHECK(client.wait_for_close(6));
  const auto waited = std::chrono::steady_clock::now() - start;
  CHECK(waited >= 1500ms);
  CHECK(waited < 5000ms);
}