// A small HTTP/1.1 server for the tests of the outbound clients: it answers canned routes and records what it was
// asked. Rust: crates/campfire/src/integrations/test_support.rs (FakeServer).
#pragma once

#include <arpa/inet.h>
#include <libdeflate.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace campfire::app::test {

struct FakeRoute {
  std::string method;
  std::string host;  // "*" for any
  std::string path;
  int status = 200;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  bool chunked = false;
  bool gzip = false;
  bool trickle = false;  // after the head, one space every 50 ms until the client hangs up
  std::chrono::milliseconds delay{0};
};

struct Received {
  std::string method;
  std::string target;
  std::vector<std::pair<std::string, std::string>> headers;
  [[nodiscard]] std::string header(const std::string& name) const {
    for (const auto& [key, value] : headers) {
      if (strcasecmp(key.c_str(), name.c_str()) == 0) return value;
    }
    return {};
  }
};

inline std::string gzip_member(const std::string& data) {
  libdeflate_compressor* c = libdeflate_alloc_compressor(9);
  std::string out(libdeflate_gzip_compress_bound(c, data.size()), '\0');
  out.resize(libdeflate_gzip_compress(c, data.data(), data.size(), out.data(), out.size()));
  libdeflate_free_compressor(c);
  return out;
}

class FakeServer {
 public:
  // `tls_cert` and `tls_key`: PEM files. With them the server speaks TLS.
  explicit FakeServer(std::vector<FakeRoute> routes, const std::string& tls_cert = {}, const std::string& tls_key = {})
      : routes_(std::move(routes)) {
    if (!tls_cert.empty()) {
      ctx_ = SSL_CTX_new(TLS_server_method());
      SSL_CTX_use_certificate_chain_file(ctx_, tls_cert.c_str());
      SSL_CTX_use_PrivateKey_file(ctx_, tls_key.c_str(), SSL_FILETYPE_PEM);
    }
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof address);
    ::listen(listener_, 64);
    socklen_t size = sizeof address;
    ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size);
    port_ = ntohs(address.sin_port);
    acceptor_ = std::thread([this] { accept_loop(); });
  }
  FakeServer(const FakeServer&) = delete;
  FakeServer& operator=(const FakeServer&) = delete;
  ~FakeServer() {
    stopping_ = true;
    acceptor_.join();
    for (auto& worker : workers_) worker.join();
    ::close(listener_);
    if (ctx_ != nullptr) SSL_CTX_free(ctx_);
  }

  [[nodiscard]] std::uint16_t port() const { return port_; }
  [[nodiscard]] std::vector<Received> received() {
    std::scoped_lock lock(mutex_);
    return received_;
  }

 private:
  struct Stream {
    int fd = -1;
    SSL* ssl = nullptr;
    ssize_t read(char* out, std::size_t n) const {
      return ssl ? SSL_read(ssl, out, static_cast<int>(n)) : ::recv(fd, out, n, 0);
    }
    bool write(const std::string& data) const {
      std::size_t done = 0;
      while (done < data.size()) {
        const ssize_t n = ssl ? SSL_write(ssl, data.data() + done, static_cast<int>(data.size() - done))
                              : ::send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
      }
      return true;
    }
  };

  void accept_loop() {
    while (!stopping_) {
      pollfd item{listener_, POLLIN, 0};
      if (::poll(&item, 1, 50) <= 0) continue;
      const int fd = ::accept(listener_, nullptr, nullptr);
      if (fd < 0) continue;
      workers_.emplace_back([this, fd] { serve(fd); });
    }
  }

  void serve(int fd) {
    Stream stream{fd, nullptr};
    if (ctx_ != nullptr) {
      stream.ssl = SSL_new(ctx_);
      SSL_set_fd(stream.ssl, fd);
      if (SSL_accept(stream.ssl) != 1) {
        SSL_free(stream.ssl);
        ::close(fd);
        return;
      }
    }
    std::string request;
    char chunk[4096];
    while (request.find("\r\n\r\n") == std::string::npos) {
      const ssize_t n = stream.read(chunk, sizeof chunk);
      if (n <= 0) break;
      request.append(chunk, static_cast<std::size_t>(n));
    }
    respond(stream, request);
    if (stream.ssl != nullptr) {
      SSL_shutdown(stream.ssl);
      SSL_free(stream.ssl);
    }
    ::close(fd);
  }

  void respond(const Stream& stream, const std::string& request) {
    Received received;
    std::size_t line_end = request.find("\r\n");
    const std::string first = request.substr(0, line_end);
    const std::size_t sp1 = first.find(' ');
    const std::size_t sp2 = first.find(' ', sp1 + 1);
    received.method = first.substr(0, sp1);
    received.target = first.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string host;
    while (line_end != std::string::npos && line_end + 2 < request.size()) {
      const std::size_t start = line_end + 2;
      line_end = request.find("\r\n", start);
      const std::string line = request.substr(start, line_end - start);
      const std::size_t colon = line.find(": ");
      if (colon == std::string::npos) continue;
      received.headers.emplace_back(line.substr(0, colon), line.substr(colon + 2));
    }
    host = received.header("host");
    if (const std::size_t colon = host.rfind(':');
        colon != std::string::npos && host.find_first_not_of("0123456789", colon + 1) == std::string::npos) {
      host = host.substr(0, colon);
    }
    {
      std::scoped_lock lock(mutex_);
      received_.push_back(received);
    }
    FakeRoute not_found{received.method, host, received.target, 404, {{"Content-Type", "text/plain"}}, "not found"};
    const FakeRoute* route = &not_found;
    for (const FakeRoute& candidate : routes_) {
      if (candidate.method == received.method && (candidate.host == host || candidate.host == "*") &&
          candidate.path == received.target) {
        route = &candidate;
        break;
      }
    }
    std::this_thread::sleep_for(route->delay);
    std::string body = route->body;
    if (route->gzip) body = gzip_member(body);
    std::string head = "HTTP/1.1 " + std::to_string(route->status) + " Status\r\n";
    bool has_length = false;
    for (const auto& [name, value] : route->headers) {
      head += name + ": " + value + "\r\n";
      if (strcasecmp(name.c_str(), "content-length") == 0) has_length = true;
    }
    if (route->gzip) head += "Content-Encoding: gzip\r\n";
    if (route->chunked)
      head += "Transfer-Encoding: chunked\r\n";
    else if (!has_length && !route->trickle)
      head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    head += "Connection: close\r\n\r\n";
    if (!stream.write(head)) return;
    if (route->trickle) {
      for (int i = 0; i < 400 && stream.write(" "); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
      return;
    }
    if (received.method == "HEAD") return;
    if (route->chunked) {
      for (std::size_t at = 0; at < body.size(); at += 65536) {
        const std::string piece = body.substr(at, 65536);
        char size[32];
        std::snprintf(size, sizeof size, "%zx\r\n", piece.size());
        if (!stream.write(size + piece + "\r\n")) return;
      }
      stream.write("0\r\n\r\n");
    } else {
      stream.write(body);
    }
  }

  std::vector<FakeRoute> routes_;
  SSL_CTX* ctx_ = nullptr;
  int listener_ = -1;
  std::uint16_t port_ = 0;
  std::atomic<bool> stopping_{false};
  std::thread acceptor_;
  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::vector<Received> received_;
};

}  // namespace campfire::app::test
