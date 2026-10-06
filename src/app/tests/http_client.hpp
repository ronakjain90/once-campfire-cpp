// A small blocking HTTP/1.1 client for the app tests.
#pragma once

#include <arpa/inet.h>
#include <doctest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace campfire::app::testing {

struct Reply {
  int status = 0;
  std::string head;
  std::string body;                                          // as sent: a gzip body stays gzip
  std::vector<std::pair<std::string, std::string>> headers;  // names in lower case, in order

  [[nodiscard]] std::string header(const std::string& name) const {
    for (const auto& [k, v] : headers) {
      if (k == name) return v;
    }
    return {};
  }
  [[nodiscard]] std::vector<std::string> header_names() const {
    std::vector<std::string> out;
    for (const auto& h : headers) out.push_back(h.first);
    return out;
  }
};

class Client {
 public:
  explicit Client(std::uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    REQUIRE(::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    timeval tv{10, 0};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  }
  ~Client() { ::close(fd_); }
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // `extra` is a list of header lines, each ending in CRLF.
  Reply request(const std::string& method, const std::string& path, const std::string& extra = {},
                const std::string& body = {}) {
    std::string text = method + " " + path + " HTTP/1.1\r\nHost: test.example\r\n" + extra;
    if (!body.empty()) text += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    text += "\r\n" + body;
    while (!text.empty()) {
      const ssize_t n = ::send(fd_, text.data(), text.size(), MSG_NOSIGNAL);
      REQUIRE(n > 0);
      text.erase(0, static_cast<std::size_t>(n));
    }
    return read_reply(method == "HEAD");
  }

 private:
  Reply read_reply(bool head_request) {
    Reply reply;
    std::size_t end = std::string::npos;
    while ((end = buffer_.find("\r\n\r\n")) == std::string::npos) REQUIRE(fill());
    reply.head = buffer_.substr(0, end);
    buffer_.erase(0, end + 4);
    reply.status = std::stoi(reply.head.substr(9, 3));
    std::size_t at = reply.head.find("\r\n");
    while (at != std::string::npos) {
      const std::size_t next = reply.head.find("\r\n", at + 2);
      const std::string line = reply.head.substr(at + 2, next == std::string::npos ? next : next - at - 2);
      const std::size_t colon = line.find(':');
      std::string name = line.substr(0, colon);
      std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
      std::string value = line.substr(colon + 1);
      if (!value.empty() && value.front() == ' ') value.erase(0, 1);
      reply.headers.emplace_back(name, value);
      at = next;
    }
    if (head_request || reply.status == 204 || reply.status == 304) return reply;
    if (const std::string length = reply.header("content-length"); !length.empty()) {
      const std::size_t n = std::stoul(length);
      while (buffer_.size() < n) REQUIRE(fill());
      reply.body = buffer_.substr(0, n);
      buffer_.erase(0, n);
    } else if (reply.header("transfer-encoding") == "chunked") {
      while (true) {
        while (buffer_.find("\r\n") == std::string::npos) REQUIRE(fill());
        const std::size_t eol = buffer_.find("\r\n");
        const std::size_t n = std::stoul(buffer_.substr(0, eol), nullptr, 16);
        while (buffer_.size() < eol + 2 + n + 2) REQUIRE(fill());
        reply.body += buffer_.substr(eol + 2, n);
        buffer_.erase(0, eol + 2 + n + 2);
        if (n == 0) break;
      }
    }
    return reply;
  }

  bool fill() {
    char chunk[16384];
    const ssize_t n = ::recv(fd_, chunk, sizeof chunk, 0);
    if (n <= 0) return false;
    buffer_.append(chunk, static_cast<std::size_t>(n));
    return true;
  }
  int fd_ = -1;
  std::string buffer_;
};

}  // namespace campfire::app::testing
