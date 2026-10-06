// The transport of a connection: plain or TLS. Rust: crates/kit/src/front/tls.rs (accept), conn.rs (IO).
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "core/log.hpp"
#include "net/worker.hpp"

namespace campfire::net {

namespace {

// The text of the last OpenSSL error, for the log. Empty if there is none.
std::string ssl_error_text() {
  const unsigned long code = ERR_get_error();
  if (code == 0) return {};
  char buffer[256];
  ERR_error_string_n(code, buffer, sizeof buffer);
  return buffer;
}

}  // namespace

Worker::Io Worker::io_read(Conn& c, char* buffer, std::size_t size, std::size_t& got) {
  got = 0;
  if (!c.ssl) {
    while (true) {
      const ssize_t n = ::read(c.fd, buffer, size);
      if (n > 0) {
        got = static_cast<std::size_t>(n);
        return Io::Ok;
      }
      if (n == 0) return Io::Closed;
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return Io::WouldBlock;
      return Io::Closed;
    }
  }
  while (true) {
    const int n = SSL_read(c.ssl.get(), buffer, static_cast<int>(size));
    if (n > 0) {
      got = static_cast<std::size_t>(n);
      return Io::Ok;
    }
    const int reason = SSL_get_error(c.ssl.get(), n);
    if (reason == SSL_ERROR_WANT_READ) {
      c.readable = false;
      return Io::WouldBlock;
    }
    if (reason == SSL_ERROR_WANT_WRITE) {
      c.writable = false;
      return Io::WouldBlock;
    }
    if (reason == SSL_ERROR_ZERO_RETURN) return Io::Closed;
    if (reason == SSL_ERROR_SYSCALL && ERR_peek_error() == 0) return Io::Closed;
    log_debug("http: TLS read error: {}", ssl_error_text());
    return Io::Closed;
  }
}

Worker::Io Worker::io_write(Conn& c, const char* data, std::size_t size, std::size_t& wrote) {
  wrote = 0;
  if (!c.ssl) {
    while (true) {
      const ssize_t n = ::send(c.fd, data, size, MSG_NOSIGNAL);
      if (n >= 0) {
        wrote = static_cast<std::size_t>(n);
        return Io::Ok;
      }
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return Io::WouldBlock;
      return Io::Closed;
    }
  }
  while (true) {
    const int n = SSL_write(c.ssl.get(), data, static_cast<int>(size));
    if (n > 0) {
      wrote = static_cast<std::size_t>(n);
      return Io::Ok;
    }
    const int reason = SSL_get_error(c.ssl.get(), n);
    if (reason == SSL_ERROR_WANT_READ) {
      c.readable = false;
      return Io::WouldBlock;
    }
    if (reason == SSL_ERROR_WANT_WRITE) {
      c.writable = false;
      return Io::WouldBlock;
    }
    log_debug("http: TLS write error: {}", ssl_error_text());
    return Io::Closed;
  }
}

void Worker::send_text(Conn& c, std::string_view text) {
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t wrote = 0;
    if (io_write(c, text.data() + at, text.size() - at, wrote) != Io::Ok || wrote == 0) return;
    at += wrote;
  }
}

void Worker::shutdown_write(Conn& c) {
  if (c.ssl) SSL_shutdown(c.ssl.get());
  ::shutdown(c.fd, SHUT_WR);
}

bool Worker::step_handshake(Conn& c) {
  ERR_clear_error();
  const int n = SSL_accept(c.ssl.get());
  if (n == 1) {
    if (front::TlsServer::is_challenge(c.ssl.get())) {
      // A TLS-ALPN-01 validation ends with its handshake (`Tls::accept` gives `None`).
      close_conn(c);
      return false;
    }
    if (front::TlsServer::protocol(c.ssl.get()) == "h2") {
      h2_start(c, false);
      return true;
    }
    c.state = ConnState::Head;
    begin_wait(c);
    return true;
  }
  const int reason = SSL_get_error(c.ssl.get(), n);
  if (reason == SSL_ERROR_WANT_READ) {
    c.readable = false;
    return false;
  }
  if (reason == SSL_ERROR_WANT_WRITE) {
    c.writable = false;
    return false;
  }
  if (reason == SSL_ERROR_WANT_X509_LOOKUP) {
    // An ACME order runs in the CertManager thread. Try again in a moment.
    arm(c, 100, now_ms_, kTimerHandshake);
    return false;
  }
  log_debug("http: TLS handshake error remote={}: {}", c.remote, ssl_error_text());
  close_conn(c);
  return false;
}

}  // namespace campfire::net