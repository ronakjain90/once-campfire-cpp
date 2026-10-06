// A small blocking HTTP/1.1 client with TLS, and a JSON reader, for the ACME client.
#include "net/front/acme_http.hpp"

#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>

namespace campfire::net::front {

std::string HttpResult::header(std::string_view lower_name) const {
  for (const auto& [name, value] : headers) {
    if (name == lower_name) return value;
  }
  return {};
}

namespace {

struct Url {
  bool tls = true;
  std::string host;
  std::string port;
  std::string target;
};

Result<Url> parse_url(std::string_view url) {
  Url out;
  if (url.starts_with("https://")) {
    url.remove_prefix(8);
  } else if (url.starts_with("http://")) {
    out.tls = false;
    url.remove_prefix(7);
  } else {
    return fail(Errc::InvalidArgument, "not an HTTP URL: " + std::string(url));
  }
  const std::size_t slash = url.find('/');
  std::string_view authority = url.substr(0, slash);
  out.target = slash == std::string_view::npos ? "/" : std::string(url.substr(slash));
  out.port = out.tls ? "443" : "80";
  if (authority.starts_with('[')) {
    const std::size_t close = authority.find(']');
    if (close == std::string_view::npos) return fail(Errc::InvalidArgument, "bad host");
    out.host = std::string(authority.substr(1, close - 1));
    if (authority.size() > close + 2) out.port = std::string(authority.substr(close + 2));
  } else {
    const std::size_t colon = authority.rfind(':');
    out.host = std::string(authority.substr(0, colon));
    if (colon != std::string_view::npos) out.port = std::string(authority.substr(colon + 1));
  }
  return out;
}

class Socket {
 public:
  Socket() = default;
  explicit Socket(int fd) : fd_(fd) {}
  Socket(Socket&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  ~Socket() {
    if (fd_ >= 0) ::close(fd_);
  }
  [[nodiscard]] int get() const noexcept { return fd_; }

 private:
  int fd_ = -1;
};

struct SslDeleter {
  void operator()(SSL* p) const noexcept { SSL_free(p); }
};
struct CtxDeleter {
  void operator()(SSL_CTX* p) const noexcept { SSL_CTX_free(p); }
};

Result<Socket> connect_to(const Url& url, int timeout_seconds) {
  addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* list = nullptr;
  if (const int rc = getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &list); rc != 0) {
    return fail(Errc::Io, "cannot resolve " + url.host + ": " + gai_strerror(rc));
  }
  std::string last_error = "no address";
  for (addrinfo* a = list; a != nullptr; a = a->ai_next) {
    Socket s(::socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol));
    if (s.get() < 0) continue;
    timeval tv{timeout_seconds, 0};
    setsockopt(s.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s.get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (::connect(s.get(), a->ai_addr, a->ai_addrlen) == 0) {
      freeaddrinfo(list);
      return s;
    }
    last_error = std::strerror(errno);
  }
  freeaddrinfo(list);
  return fail(Errc::Io, "cannot connect to " + url.host + ":" + url.port + ": " + last_error);
}

std::string dechunk(std::string_view data) {
  std::string out;
  while (!data.empty()) {
    const std::size_t eol = data.find("\r\n");
    if (eol == std::string_view::npos) break;
    std::size_t size = 0;
    std::from_chars(data.data(), data.data() + eol, size, 16);
    data.remove_prefix(eol + 2);
    if (size == 0 || size > data.size()) {
      if (size > 0) out.append(data);
      break;
    }
    out.append(data.substr(0, size));
    data.remove_prefix(std::min(data.size(), size + 2));
  }
  return out;
}

}  // namespace

Result<HttpResult> http_request(std::string_view method, std::string_view url_text,
                                const std::vector<std::pair<std::string, std::string>>& headers, std::string_view body,
                                const std::string& ca_file, int timeout_seconds) {
  auto url = parse_url(url_text);
  if (!url) return std::unexpected(url.error());
  auto socket = connect_to(*url, timeout_seconds);
  if (!socket) return std::unexpected(socket.error());

  std::unique_ptr<SSL_CTX, CtxDeleter> ctx;
  std::unique_ptr<SSL, SslDeleter> ssl;
  if (url->tls) {
    ctx.reset(SSL_CTX_new(TLS_client_method()));
    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);
    const bool loaded = ca_file.empty() ? SSL_CTX_set_default_verify_paths(ctx.get()) == 1
                                        : SSL_CTX_load_verify_file(ctx.get(), ca_file.c_str()) == 1;
    if (!loaded) return fail(Errc::Io, "cannot load the root certificates");
    ssl.reset(SSL_new(ctx.get()));
    SSL_set_fd(ssl.get(), socket->get());
    SSL_set_tlsext_host_name(ssl.get(), url->host.c_str());
    SSL_set1_host(ssl.get(), url->host.c_str());
    if (SSL_connect(ssl.get()) != 1) {
      ERR_clear_error();
      return fail(Errc::Io, "TLS handshake with " + url->host + " failed");
    }
  }
  std::string request = std::string(method) + " " + url->target + " HTTP/1.1\r\nhost: " + url->host + "\r\n" +
                        "user-agent: campfire-front\r\nconnection: close\r\n";
  for (const auto& [name, value] : headers) request += name + ": " + value + "\r\n";
  if (!body.empty() || method == "POST") request += "content-length: " + std::to_string(body.size()) + "\r\n";
  request += "\r\n";
  request.append(body);

  const auto write_all = [&](std::string_view data) {
    while (!data.empty()) {
      const int n = ssl ? SSL_write(ssl.get(), data.data(), static_cast<int>(data.size()))
                        : static_cast<int>(::send(socket->get(), data.data(), data.size(), MSG_NOSIGNAL));
      if (n <= 0) return false;
      data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
  };
  if (!write_all(request)) return fail(Errc::Io, "cannot send the request to " + url->host);

  std::string raw;
  char buffer[16384];
  while (true) {
    const int n = ssl ? SSL_read(ssl.get(), buffer, sizeof buffer) : static_cast<int>(::recv(socket->get(), buffer, sizeof buffer, 0));
    if (n <= 0) break;
    raw.append(buffer, static_cast<std::size_t>(n));
  }
  const std::size_t head_end = raw.find("\r\n\r\n");
  if (head_end == std::string::npos) return fail(Errc::Io, "no answer from " + url->host);
  HttpResult result;
  const std::string head = raw.substr(0, head_end);
  std::size_t line_end = head.find("\r\n");
  const std::string status_line = head.substr(0, line_end);
  const std::size_t space = status_line.find(' ');
  if (space == std::string::npos) return fail(Errc::Parse, "bad status line");
  result.status = std::atoi(status_line.c_str() + space + 1);
  while (line_end != std::string::npos) {
    const std::size_t next = head.find("\r\n", line_end + 2);
    const std::string line = head.substr(line_end + 2, next == std::string::npos ? std::string::npos : next - line_end - 2);
    line_end = next;
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = line.substr(0, colon);
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string value = line.substr(colon + 1);
    value.erase(0, value.find_first_not_of(" \t"));
    result.headers.emplace_back(std::move(name), std::move(value));
  }
  std::string_view payload = std::string_view(raw).substr(head_end + 4);
  result.body = result.header("transfer-encoding") == "chunked" ? dechunk(payload) : std::string(payload);
  return result;
}

// --- JSON ---

class JsonParser {
 public:
  explicit JsonParser(std::string_view text) : text_(text) {}

  Result<Json> parse() {
    auto value = value_();
    if (!value) return value;
    skip();
    if (pos_ != text_.size()) return fail(Errc::Parse, "trailing text in JSON");
    return value;
  }

 private:
  void skip() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_])) != 0) ++pos_;
  }
  Result<Json> value_() {
    skip();
    if (pos_ >= text_.size()) return fail(Errc::Parse, "unexpected end of JSON");
    const char c = text_[pos_];
    Json out;
    if (c == '{') {
      ++pos_;
      Json::Object object;
      skip();
      if (pos_ < text_.size() && text_[pos_] == '}') {
        ++pos_;
      } else {
        while (true) {
          skip();
          auto key = string_();
          if (!key) return std::unexpected(key.error());
          skip();
          if (pos_ >= text_.size() || text_[pos_] != ':') return fail(Errc::Parse, "expected ':' in JSON");
          ++pos_;
          auto member = value_();
          if (!member) return member;
          object.emplace_back(std::move(*key), std::move(*member));
          skip();
          if (pos_ < text_.size() && text_[pos_] == ',') {
            ++pos_;
            continue;
          }
          if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            break;
          }
          return fail(Errc::Parse, "expected ',' or '}' in JSON");
        }
      }
      out.data_ = std::move(object);
    } else if (c == '[') {
      ++pos_;
      Json::Array array;
      skip();
      if (pos_ < text_.size() && text_[pos_] == ']') {
        ++pos_;
      } else {
        while (true) {
          auto item = value_();
          if (!item) return item;
          array.push_back(std::move(*item));
          skip();
          if (pos_ < text_.size() && text_[pos_] == ',') {
            ++pos_;
            continue;
          }
          if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            break;
          }
          return fail(Errc::Parse, "expected ',' or ']' in JSON");
        }
      }
      out.data_ = std::move(array);
    } else if (c == '"') {
      auto text = string_();
      if (!text) return std::unexpected(text.error());
      out.data_ = std::move(*text);
    } else if (text_.substr(pos_).starts_with("true")) {
      pos_ += 4;
      out.data_ = true;
    } else if (text_.substr(pos_).starts_with("false")) {
      pos_ += 5;
      out.data_ = false;
    } else if (text_.substr(pos_).starts_with("null")) {
      pos_ += 4;
    } else {
      const std::size_t start = pos_;
      while (pos_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0 || std::strchr("+-.eE", text_[pos_]) != nullptr)) ++pos_;
      if (start == pos_) return fail(Errc::Parse, "bad JSON value");
      out.data_ = std::strtod(std::string(text_.substr(start, pos_ - start)).c_str(), nullptr);
    }
    return out;
  }
  Result<std::string> string_() {
    if (pos_ >= text_.size() || text_[pos_] != '"') return fail(Errc::Parse, "expected a string in JSON");
    ++pos_;
    std::string out;
    while (pos_ < text_.size() && text_[pos_] != '"') {
      char c = text_[pos_++];
      if (c == '\\' && pos_ < text_.size()) {
        c = text_[pos_++];
        switch (c) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'u': {
            unsigned value = 0;
            if (pos_ + 4 > text_.size()) return fail(Errc::Parse, "bad \\u escape");
            std::from_chars(text_.data() + pos_, text_.data() + pos_ + 4, value, 16);
            pos_ += 4;
            if (value < 0x80) {
              out += static_cast<char>(value);
            } else if (value < 0x800) {
              out += static_cast<char>(0xC0 | (value >> 6));
              out += static_cast<char>(0x80 | (value & 0x3F));
            } else {
              out += static_cast<char>(0xE0 | (value >> 12));
              out += static_cast<char>(0x80 | ((value >> 6) & 0x3F));
              out += static_cast<char>(0x80 | (value & 0x3F));
            }
            break;
          }
          default: out += c; break;
        }
      } else {
        out += c;
      }
    }
    if (pos_ >= text_.size()) return fail(Errc::Parse, "unterminated string in JSON");
    ++pos_;
    return out;
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

Result<Json> Json::parse(std::string_view text) { return JsonParser(text).parse(); }

const Json* Json::find(std::string_view key) const noexcept {
  if (const auto* object = std::get_if<Object>(&data_)) {
    for (const auto& [k, v] : *object) {
      if (k == key) return &v;
    }
  }
  return nullptr;
}

std::string_view Json::as_string() const noexcept {
  if (const auto* s = std::get_if<std::string>(&data_)) return *s;
  return {};
}

std::string_view Json::str(std::string_view key) const noexcept {
  const Json* v = find(key);
  return v != nullptr ? v->as_string() : std::string_view{};
}

const Json::Array& Json::array() const noexcept {
  static const Array kEmpty;
  if (const auto* a = std::get_if<Array>(&data_)) return *a;
  return kEmpty;
}

}  // namespace campfire::net::front
