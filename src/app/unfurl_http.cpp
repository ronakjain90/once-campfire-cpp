// Net::HTTP for the link unfurl. Rust: crates/campfire/src/integrations/net/http.rs.
#include "app/unfurl_http.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <libdeflate.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "compat/ruby.hpp"

namespace campfire::app::unfurl {

namespace {

// The `Accept-Encoding` that `Net::HTTP` adds to a request whose response has a body, and then decodes itself.
constexpr std::string_view kAcceptEncoding = "gzip;q=1.0,deflate;q=0.6,identity;q=0.3";
constexpr std::size_t kMaxHead = std::size_t{64} * 1024;
constexpr std::size_t kMaxChunkLine = 4096;

Error timeout_error(std::string message) {
  return Error{Errc::Timeout, std::move(message)};
}
Error io_error(std::string message) {
  return Error{Errc::Io, std::move(message)};
}

struct SslDeleter {
  void operator()(SSL* ssl) const { SSL_free(ssl); }
};
struct CtxDeleter {
  void operator()(SSL_CTX* ctx) const { SSL_CTX_free(ctx); }
};

std::string ssl_error_text() {
  const unsigned long code = ERR_get_error();
  if (code == 0) return "TLS error";
  char buffer[256] = {};
  ERR_error_string_n(code, buffer, sizeof buffer);
  ERR_clear_error();
  return buffer;
}

}  // namespace

class Connection {
 public:
  Connection() = default;
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  ~Connection() {
    ssl_.reset();
    ctx_.reset();
    if (fd_ >= 0) ::close(fd_);
  }

  Timeouts timeouts;
  Clock::time_point deadline;
  std::string buffer;  // bytes read and not used yet

  Status connect_to(const Network& network, const Endpoint& endpoint) {
    const auto open_deadline = std::min(deadline, Clock::now() + timeouts.open);
    // `TCPSocket.open(host, port)`: each address in turn, until one connects.
    std::vector<std::string> addresses{endpoint.pinned_ip};
    addresses.insert(addresses.end(), endpoint.more_ips.begin(), endpoint.more_ips.end());
    Status last;
    for (const std::string& address : addresses) {
      std::string ip = address;
      std::uint16_t port = endpoint.port;
      if (network.dial_override) network.dial_override(ip, port);
      last = connect_address(ip, port, open_deadline);
      if (last) break;
      if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
      }
    }
    if (!last) return last;
    if (!endpoint.https) return {};
    return start_tls(network, endpoint, open_deadline);
  }

  Status connect_address(const std::string& ip, std::uint16_t port, Clock::time_point open_deadline) {
    sockaddr_storage address{};
    socklen_t length = 0;
    if (auto* v4 = reinterpret_cast<sockaddr_in*>(&address); inet_pton(AF_INET, ip.c_str(), &v4->sin_addr) == 1) {
      v4->sin_family = AF_INET;
      v4->sin_port = htons(port);
      length = sizeof(sockaddr_in);
    } else if (auto* v6 = reinterpret_cast<sockaddr_in6*>(&address);
               inet_pton(AF_INET6, ip.c_str(), &v6->sin6_addr) == 1) {
      v6->sin6_family = AF_INET6;
      v6->sin6_port = htons(port);
      length = sizeof(sockaddr_in6);
    } else {
      return std::unexpected(io_error("bad address " + ip));
    }
    fd_ = ::socket(address.ss_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd_ < 0) return std::unexpected(io_error(std::strerror(errno)));
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), length) != 0) {
      if (errno != EINPROGRESS) return std::unexpected(io_error(std::strerror(errno)));
      if (!wait(POLLOUT, open_deadline)) return std::unexpected(timeout_error("execution expired"));
      int error = 0;
      socklen_t size = sizeof error;
      ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &size);
      if (error != 0) return std::unexpected(io_error(std::strerror(error)));
    }
    return {};
  }

  Status write_all(std::string_view data) {
    while (!data.empty()) {
      const ssize_t n = send_some(data);
      if (n > 0) {
        data.remove_prefix(static_cast<std::size_t>(n));
      } else if (n == kWouldBlockWrite) {
        if (!wait(POLLOUT, std::min(deadline, Clock::now() + timeouts.read))) {
          return std::unexpected(timeout_error("Net::ReadTimeout"));
        }
      } else if (n == kWouldBlockRead) {
        if (!wait(POLLIN, std::min(deadline, Clock::now() + timeouts.read))) {
          return std::unexpected(timeout_error("Net::ReadTimeout"));
        }
      } else {
        return std::unexpected(io_error("write failed"));
      }
    }
    return {};
  }

  // Appends what arrives to `buffer`. Gives the number of bytes (0 at the end of the stream).
  Result<std::size_t> fill() {
    char chunk[16384];
    for (;;) {
      const ssize_t n = recv_some(chunk, sizeof chunk);
      if (n > 0) {
        buffer.append(chunk, static_cast<std::size_t>(n));
        return static_cast<std::size_t>(n);
      }
      if (n == 0) return std::size_t{0};
      if (n == kWouldBlockRead || n == kWouldBlockWrite) {
        const short events = n == kWouldBlockRead ? POLLIN : POLLOUT;
        if (!wait(events, std::min(deadline, Clock::now() + timeouts.read))) {
          return std::unexpected(timeout_error("Net::ReadTimeout"));
        }
        continue;
      }
      return std::unexpected(io_error("read failed"));
    }
  }

 private:
  static constexpr ssize_t kWouldBlockRead = -2;
  static constexpr ssize_t kWouldBlockWrite = -3;

  bool wait(short events, Clock::time_point until) const {
    for (;;) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now());
      if (left.count() <= 0) return false;
      pollfd item{fd_, events, 0};
      const int ready = ::poll(&item, 1, static_cast<int>(std::min<std::int64_t>(left.count(), 1000)));
      if (ready > 0) return true;
      if (ready < 0 && errno != EINTR) return false;
    }
  }

  ssize_t send_some(std::string_view data) {
    if (!ssl_) {
      const ssize_t n = ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
      if (n >= 0) return n;
      return errno == EAGAIN || errno == EINTR ? kWouldBlockWrite : -1;
    }
    const int n = SSL_write(ssl_.get(), data.data(), static_cast<int>(std::min<std::size_t>(data.size(), 1 << 20)));
    if (n > 0) return n;
    return map_ssl(n);
  }

  ssize_t recv_some(char* out, std::size_t size) {
    if (!ssl_) {
      const ssize_t n = ::recv(fd_, out, size, 0);
      if (n >= 0) return n;
      return errno == EAGAIN || errno == EINTR ? kWouldBlockRead : -1;
    }
    const int n = SSL_read(ssl_.get(), out, static_cast<int>(size));
    if (n > 0) return n;
    return map_ssl(n);
  }

  ssize_t map_ssl(int result) {
    switch (SSL_get_error(ssl_.get(), result)) {
      case SSL_ERROR_WANT_READ: return kWouldBlockRead;
      case SSL_ERROR_WANT_WRITE: return kWouldBlockWrite;
      case SSL_ERROR_ZERO_RETURN: return 0;
      case SSL_ERROR_SYSCALL:
        // An end of stream without close_notify: take it as the end of the body.
        return result == 0 ? 0 : -1;
      default: return -1;
    }
  }

  Status start_tls(const Network& network, const Endpoint& endpoint, Clock::time_point until) {
    ctx_.reset(SSL_CTX_new(TLS_client_method()));
    if (!ctx_) return std::unexpected(io_error(ssl_error_text()));
    SSL_CTX_set_min_proto_version(ctx_.get(), TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx_.get(), SSL_VERIFY_PEER, nullptr);
    const int loaded = network.ca_file.empty()
                           ? SSL_CTX_set_default_verify_paths(ctx_.get())
                           : SSL_CTX_load_verify_locations(ctx_.get(), network.ca_file.c_str(), nullptr);
    if (loaded != 1) return std::unexpected(io_error("cannot load CA certificates"));
    ssl_.reset(SSL_new(ctx_.get()));
    if (!ssl_) return std::unexpected(io_error(ssl_error_text()));
    std::string name = endpoint.host;
    if (name.size() > 1 && name.front() == '[' && name.back() == ']') name = name.substr(1, name.size() - 2);
    SSL_set_fd(ssl_.get(), fd_);
    SSL_set_tlsext_host_name(ssl_.get(), name.c_str());
    SSL_set1_host(ssl_.get(), name.c_str());
    for (;;) {
      const int result = SSL_connect(ssl_.get());
      if (result == 1) return {};
      const int code = SSL_get_error(ssl_.get(), result);
      if (code != SSL_ERROR_WANT_READ && code != SSL_ERROR_WANT_WRITE) {
        return std::unexpected(io_error("SSL error: " + ssl_error_text()));
      }
      if (!wait(code == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, until)) {
        return std::unexpected(timeout_error("execution expired"));
      }
    }
  }

  int fd_ = -1;
  std::unique_ptr<SSL_CTX, CtxDeleter> ctx_;
  std::unique_ptr<SSL, SslDeleter> ssl_;
};

Response::Response(Response&&) noexcept = default;
Response& Response::operator=(Response&&) noexcept = default;
Response::~Response() = default;

std::optional<std::string> Response::header(std::string_view name) const {
  std::optional<std::string> joined;
  for (const auto& [key, value] : headers) {
    if (key != name) continue;
    if (joined) {
      *joined += ", " + value;
    } else {
      joined = value;
    }
  }
  return joined;
}

std::optional<std::string> Response::content_type() const {
  const auto value = header("content-type");
  if (!value) return std::nullopt;
  const std::string_view media = std::string_view(*value).substr(0, value->find(';'));
  const std::size_t slash = media.find('/');
  if (slash == std::string_view::npos) return std::string(compat::strip(media));
  // `split('/')`: the main type and the first part after it.
  const std::string_view rest = media.substr(slash + 1);
  return std::string(compat::strip(media.substr(0, slash))) + "/" +
         std::string(compat::strip(rest.substr(0, rest.find('/'))));
}

Result<std::optional<std::uint64_t>> Response::content_length() const {
  const auto value = header("content-length");
  if (!value) return std::optional<std::uint64_t>{};
  std::size_t i = 0;
  while (i < value->size() && !(((*value)[i]) >= '0' && (*value)[i] <= '9')) ++i;
  if (i == value->size()) return fail(Errc::Parse, "wrong Content-Length format");
  std::uint64_t number = 0;
  bool overflow = false;
  for (; i < value->size() && (*value)[i] >= '0' && (*value)[i] <= '9'; ++i) {
    const auto digit = static_cast<std::uint64_t>((*value)[i] - '0');
    if (number > (UINT64_MAX - digit) / 10) overflow = true;
    number = overflow ? UINT64_MAX : number * 10 + digit;
  }
  return std::optional<std::uint64_t>(number);
}

namespace {

// Reads the framed body into `out`, up to `limit` bytes. `true` in the second member: it is longer.
Result<Body> read_framed(Connection& c, bool chunked, std::optional<std::uint64_t> length, std::size_t limit) {
  Body body;
  const auto take = [&](std::size_t n) {
    body.bytes.append(c.buffer, 0, n);
    c.buffer.erase(0, n);
  };
  if (chunked) {
    for (;;) {
      std::size_t eol;
      while ((eol = c.buffer.find("\r\n")) == std::string::npos) {
        // A chunk size line is short. Without a limit, a reply with no line end fills the memory until the deadline.
        if (c.buffer.size() > kMaxChunkLine) return fail(Errc::Parse, "wrong chunk size line");
        auto got = c.fill();
        if (!got) return std::unexpected(got.error());
        if (*got == 0) return fail(Errc::Parse, "end of file reached");
      }
      std::string line = c.buffer.substr(0, eol);
      line = line.substr(0, line.find(';'));
      std::uint64_t size = 0;
      for (const char ch : compat::strip(line)) {
        int digit;
        if (ch >= '0' && ch <= '9')
          digit = ch - '0';
        else if (ch >= 'a' && ch <= 'f')
          digit = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F')
          digit = ch - 'A' + 10;
        else
          return fail(Errc::Parse, "wrong chunk size line");
        if (size > (UINT64_MAX >> 5)) return fail(Errc::Parse, "wrong chunk size line");
        size = size * 16 + static_cast<std::uint64_t>(digit);
      }
      c.buffer.erase(0, eol + 2);
      if (size == 0) return body;  // the trailer is not read: the connection closes
      if (body.bytes.size() + size > limit) {
        body.too_large = true;
        return body;
      }
      while (c.buffer.size() < size + 2) {
        auto got = c.fill();
        if (!got) return std::unexpected(got.error());
        if (*got == 0) return fail(Errc::Parse, "end of file reached");
      }
      take(static_cast<std::size_t>(size));
      c.buffer.erase(0, 2);
    }
  }
  if (length) {
    if (*length > limit) {
      body.too_large = true;
      return body;
    }
    while (c.buffer.size() < *length) {
      auto got = c.fill();
      if (!got) return std::unexpected(got.error());
      if (*got == 0) return fail(Errc::Parse, "end of file reached");
    }
    take(static_cast<std::size_t>(*length));
    return body;
  }
  // Until the end of the stream.
  for (;;) {
    if (c.buffer.size() > limit) {
      body.too_large = true;
      return body;
    }
    auto got = c.fill();
    if (!got) return std::unexpected(got.error());
    if (*got == 0) break;
  }
  take(c.buffer.size());
  return body;
}

// `Zlib::Inflate.new(32 + Zlib::MAX_WBITS)`: gzip or zlib, from the header of the data.
Result<Body> inflate(const std::string& packed, std::size_t limit) {
  struct Decompressor {
    libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    ~Decompressor() { libdeflate_free_decompressor(d); }
  } decompressor;
  Body body;
  body.bytes.resize(limit + 1);
  std::size_t total = 0;
  const auto is_gzip = [&](std::size_t at) {
    return packed.size() >= at + 2 && static_cast<unsigned char>(packed[at]) == 0x1F &&
           static_cast<unsigned char>(packed[at + 1]) == 0x8B;
  };
  if (!is_gzip(0)) {
    std::size_t actual = 0;
    const auto result = libdeflate_zlib_decompress(decompressor.d, packed.data(), packed.size(), body.bytes.data(),
                                                   body.bytes.size(), &actual);
    if (result == LIBDEFLATE_INSUFFICIENT_SPACE) return Body{{}, true};
    if (result != LIBDEFLATE_SUCCESS) return fail(Errc::Parse, "incorrect header check");
    total = actual;
  } else {
    // `Zlib::GzipReader` style: the members one after the other.
    std::size_t at = 0;
    while (at < packed.size()) {
      if (!is_gzip(at)) return fail(Errc::Parse, "not in gzip format");
      std::size_t used = 0;
      std::size_t made = 0;
      const auto result =
          libdeflate_gzip_decompress_ex(decompressor.d, packed.data() + at, packed.size() - at,
                                        body.bytes.data() + total, body.bytes.size() - total, &used, &made);
      if (result == LIBDEFLATE_INSUFFICIENT_SPACE) return Body{{}, true};
      if (result != LIBDEFLATE_SUCCESS) return fail(Errc::Parse, "invalid compressed data");
      at += used;
      total += made;
    }
  }
  if (total > limit) return Body{{}, true};
  body.bytes.resize(total);
  return body;
}

}  // namespace

Result<Body> Response::read_body(std::size_t limit) {
  if (head_ || (status >= 100 && status < 200) || status == 204 || status == 304) return Body{};
  Connection& c = *connection_;
  const auto encoding = header("transfer-encoding");
  const bool chunked = encoding && encoding->find("chunked") != std::string::npos;
  std::optional<std::uint64_t> length;
  if (!chunked) {
    auto parsed = content_length();
    if (!parsed) return std::unexpected(parsed.error());
    length = *parsed;
  }
  std::string coding;
  if (decode_content_ && !header("content-range")) {
    if (const auto value = header("content-encoding")) {
      for (const char ch : *value) coding += static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    }
  }
  const bool packed = coding == "gzip" || coding == "x-gzip" || coding == "deflate";
  // A packed body is read up to a little more than the limit, then inflated against the limit.
  auto body = read_framed(c, chunked, length, packed ? limit + 65536 : limit);
  if (!body || body->too_large || !packed) return body;
  return inflate(body->bytes, limit);
}

Result<Response> exchange(const Network& network, const Endpoint& endpoint, const Request& request,
                          const Timeouts& timeouts, Clock::time_point deadline) {
  Response response;
  response.connection_ = std::make_unique<Connection>();
  Connection& c = *response.connection_;
  c.timeouts = timeouts;
  c.deadline = deadline;
  if (auto connected = c.connect_to(network, endpoint); !connected) return std::unexpected(connected.error());
  response.head_ = request.method == "HEAD";
  std::string text = request.method + " " + request.target + " HTTP/1.1\r\n";
  if (request.headers) {
    response.decode_content_ = request.decode_content;
    for (const auto& [name, value] : *request.headers) text += name + ": " + value + "\r\n";
    text += "\r\n";
    text += request.body;
  } else {
    response.decode_content_ = !response.head_;
    // `Net::HTTP::Get.new(uri)`, then `request` on a connection that was not started.
    text += "Accept-Encoding: " + std::string(kAcceptEncoding) + "\r\n";
    text += "Accept: */*\r\nUser-Agent: Ruby\r\nHost: " + request.host_header + "\r\nConnection: close\r\n\r\n";
  }
  if (auto sent = c.write_all(text); !sent) return std::unexpected(sent.error());
  std::size_t end;
  while ((end = c.buffer.find("\r\n\r\n")) == std::string::npos) {
    if (c.buffer.size() > kMaxHead) return fail(Errc::Parse, "response head too long");
    auto got = c.fill();
    if (!got) return std::unexpected(got.error());
    if (*got == 0) return fail(Errc::Parse, "end of file reached");
  }
  const std::string head = c.buffer.substr(0, end);
  c.buffer.erase(0, end + 4);
  std::size_t line_end = head.find("\r\n");
  const std::string status_line = head.substr(0, line_end);
  const std::size_t first_space = status_line.find(' ');
  if (status_line.size() < 12 || status_line.compare(0, 5, "HTTP/") != 0 || first_space == std::string::npos) {
    return fail(Errc::Parse, "wrong status line");
  }
  response.status = std::atoi(status_line.c_str() + first_space + 1);
  if (const std::size_t second = status_line.find(' ', first_space + 1); second != std::string::npos) {
    response.reason = status_line.substr(second + 1);
  }
  while (line_end != std::string::npos) {
    const std::size_t start = line_end + 2;
    line_end = head.find("\r\n", start);
    const std::string_view line =
        std::string_view(head).substr(start, line_end == std::string::npos ? std::string::npos : line_end - start);
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) continue;
    std::string name(line.substr(0, colon));
    for (char& ch : name) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    response.headers.emplace_back(std::move(name), std::string(compat::strip(line.substr(colon + 1))));
  }
  return response;
}

}  // namespace campfire::app::unfurl
