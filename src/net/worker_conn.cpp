// Connection state machine of the HTTP/1.1 server. Rust: crates/kit/src/front/conn.rs (hyper, Go timeouts).
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <string>

#include "core/log.hpp"
#include "net/front.hpp"
#include "net/front/static_files.hpp"
#include "net/worker.hpp"

namespace campfire::net {

namespace {

constexpr std::uint64_t kLingerMs = 2000;
constexpr std::size_t kReadStep = 4096;
constexpr std::size_t kMaxIov = 32;

}  // namespace

void Worker::arm(Conn& c, std::int64_t period_ms, std::uint64_t since_ms, TimerKind kind) {
  if (period_ms <= 0) {
    wheel_.cancel(c.timer);
    return;
  }
  const std::uint64_t due = since_ms + static_cast<std::uint64_t>(period_ms);
  wheel_.arm(c.timer, now_ms_, due > now_ms_ ? due - now_ms_ : 1, kind);
}

void Worker::pump(Conn& c) {
  while (!c.closed) {
    bool progress = false;
    switch (c.state) {
      case ConnState::Handshake: progress = step_handshake(c); break;
      case ConnState::Http2: progress = h2_step(c); break;
      case ConnState::Head: progress = step_head(c); break;
      case ConnState::Body: progress = step_body(c); break;
      case ConnState::Handling:
        if (c.handler_active) return;
        c.task = Task<void>{};
        begin_write(c);
        progress = true;
        break;
      case ConnState::Writing: progress = step_write(c); break;
      case ConnState::Linger: progress = step_linger(c); break;
    }
    if (!progress) return;
  }
}

void Worker::begin_wait(Conn& c) {
  c.state = ConnState::Head;
  c.scanned = 0;
  if (c.rbuf.empty()) c.rbuf.release();
  // Over HTTP/1, the header timer of hyper runs while the connection waits: the shorter of the
  // idle timeout and the read timeout (Rust README, "Front server").
  std::int64_t period = options_.idle_timeout_ms;
  if (options_.read_timeout_ms > 0 && (period <= 0 || options_.read_timeout_ms < period))
    period = options_.read_timeout_ms;
  arm(c, period, now_ms_, kTimerHead);
}

bool Worker::fill(Conn& c) {
  c.rbuf.reserve(c.rbuf.size() + kReadStep);
  const std::size_t room = c.rbuf.room();
  std::size_t got = 0;
  const Io result = io_read(c, c.rbuf.tail(), room, got);
  if (result != Io::Ok) {
    if (got != 0) {
      c.rbuf.commit(got);
      return true;
    }
    if (result == Io::WouldBlock) {
      c.readable = false;  // the socket has no more bytes: wait for the next event
      return false;
    }
    close_conn(c);
    return false;
  }
  c.rbuf.commit(got);
  if (got < room && !c.ssl) c.readable = false;  // TLS: a record is not all the bytes
  return true;
}

bool Worker::step_head(Conn& c) {
  if (c.h2c) {
    // `Protocol::Auto`: the first bytes decide between HTTP/1.1 and HTTP/2 (the preface).
    const std::size_t compared = std::min(c.rbuf.size(), kH2Preface.size());
    if (std::string_view(c.rbuf.data(), compared) == kH2Preface.substr(0, compared)) {
      if (c.rbuf.size() < kH2Preface.size()) return c.readable && fill(c);
      h2_start(c);
      return true;
    }
    c.h2c = false;
  }
  if (c.rbuf.empty()) {
    if (!c.readable || !fill(c)) return false;
  }
  if (!c.arena) c.arena = take_arena();
  c.arena->reset();  // drops the header list of an earlier try
  const HeadResult result =
      parse_head(std::string_view(c.rbuf.data(), c.rbuf.size()), c.scanned, *c.arena, options_.parser, c.head);
  switch (result.status) {
    case HeadStatus::NeedMore: c.scanned = c.rbuf.size(); return c.readable && fill(c);
    case HeadStatus::Error: reply_error(c, result.error_status); return true;
    case HeadStatus::Ok: break;
  }
  c.request_start_ms = now_ms_;
  const std::uint64_t limit = options_.max_request_body;
  if (c.head.body_kind == BodyKind::Length &&
      ((limit != 0 && c.head.content_length > limit) || c.head.content_length > options_.max_buffered_body)) {
    reply_error(c, 413);
    return true;
  }
  c.state = ConnState::Body;
  c.continue_sent = false;
  c.body_size = 0;
  if (c.head.body_kind != BodyKind::None) arm(c, options_.read_timeout_ms, c.request_start_ms, kTimerBody);
  return true;
}

bool Worker::step_body(Conn& c) {
  const std::size_t head_size = c.head.head_size;
  switch (c.head.body_kind) {
    case BodyKind::None: break;
    case BodyKind::Length: {
      const std::size_t total = head_size + static_cast<std::size_t>(c.head.content_length);
      if (c.rbuf.size() < total) {
        if (c.head.expect_continue && !c.continue_sent) send_continue(c);
        return c.readable && fill(c);
      }
      c.body_size = static_cast<std::size_t>(c.head.content_length);
      break;
    }
    case BodyKind::Chunked: {
      if (!c.chunked) {
        std::uint64_t cap = options_.max_buffered_body;
        if (options_.max_request_body != 0) cap = std::min(cap, options_.max_request_body);
        c.chunked.emplace(cap);
      }
      std::size_t have = c.rbuf.size() - head_size;
      if (have > c.chunked->decoded()) {  // new bytes came
        const ChunkedBody::Status status = c.chunked->feed(c.rbuf.data() + head_size, have);
        c.rbuf.resize(head_size + have);
        if (status == ChunkedBody::Status::Error) {
          reply_error(c, 400);
          return true;
        }
        if (status == ChunkedBody::Status::TooLarge) {
          reply_error(c, 413);
          return true;
        }
        if (status == ChunkedBody::Status::Done) {
          c.body_size = c.chunked->decoded();
          break;
        }
      }
      if (c.head.expect_continue && !c.continue_sent) send_continue(c);
      return c.readable && fill(c);
    }
  }
  c.head.request.body = std::string_view(c.rbuf.data() + head_size, c.body_size);
  dispatch(c);
  return true;
}

void Worker::send_continue(Conn& c) {
  c.continue_sent = true;
  static constexpr std::string_view kText = "HTTP/1.1 100 Continue\r\n\r\n";
  send_text(c, kText);
}

Task<void> Worker::serve(Conn& c) {
  std::optional<Response> response;
  try {
    if (c.handler != nullptr) {
      response.emplace(co_await c.handler(*c.ctx));
    } else {
      response.emplace(c.ctx->response(404));
    }
  } catch (const std::exception& error) {
    log_error("handler for {} {} failed: {}", c.head.request.method_text, c.head.request.path, error.what());
  } catch (...) {
    log_error("handler for {} {} failed", c.head.request.method_text, c.head.request.path);
  }
  if (!response) {
    // Rust: a panic in a handler gives a 500.
    response.emplace(c.ctx->response(500));
    response->add("content-type", "text/html; charset=UTF-8");
    response->add("content-length", "0");
  }
  handler_finished(c, std::move(*response));
}

void Worker::handler_finished(Conn& c, Response&& response) {
  c.response.emplace(std::move(response));
  c.handler_active = false;
  if (!c.in_start) finished_.push_back(&c);
}

void Worker::dispatch(Conn& c) {
  Request& request = c.head.request;
  request.via_front = c.via_front;
  request.remote_ip = std::string_view(c.remote, c.remote_size);
  c.consumed = c.head.head_size + c.body_size;
  c.close_after = !request.keep_alive || stopping_.load();
  c.ctx.emplace(*this, *c.arena, request);
  c.handler = app_.not_found;
  if (app_.routes != nullptr) {
    const Match match = match_route(*app_.routes, request.method, request.path);
    if (match) {
      c.ctx->params = match.params;
      c.handler = match.route->handler;
    }
  }
  c.state = ConnState::Handling;
  arm(c, options_.write_timeout_ms, c.request_start_ms, kTimerWrite);
  if (c.redirect_mode) {
    // With TLS on, the HTTP port runs `httpRedirectHandler`, not the front pipeline
    // (Rust: front.rs `serve_plain(http, redirect, ...)`).
    Response response = front::http_port_response(options_.tls->certs(), request, c.arena->resource());
    c.close_after = true;
    if (!response.has("date")) {
      char buffer[32];
      response.add_copy("date", http_date_now(buffer));
    }
    c.response.emplace(std::move(response));
    return;
  }
  if (c.via_front && options_.front) {
    options_.front->begin(c.front_state, request);
    if (c.front_state.status == front::CacheStatus::Hit) {
      c.response.emplace(options_.front->hit_response(c.front_state, request, c.arena->resource()));
      return;
    }
    options_.front->proxied(request, *c.arena, c.tls);
  }
  if (options_.serve_static) {
    if (std::optional<Response> served = front::serve_static(*c.ctx)) {
      if (options_.after_static) options_.after_static(*c.ctx, *served);
      c.response.emplace(std::move(*served));
      return;
    }
  }
  c.handler_active = true;
  c.in_start = true;
  c.task = serve(c);
  c.task.start();
  c.in_start = false;
}

void Worker::begin_write(Conn& c) {
  Response& response = *c.response;
  const Request& request = c.head.request;
  if (c.via_front && options_.front) {
    options_.front->finish(c.front_state, request, response);
  } else {
    suppress_bodiless_headers(response);
  }
  WireOptions wire_options;
  wire_options.head_only = request.method == Method::Head;
  wire_options.close = c.close_after;
  wire_options.keep_alive_header = request.minor_version == 0 && request.keep_alive && !c.close_after;
  wire_options.http_minor = request.minor_version;
  c.wire.emplace(c.arena->resource(), response, wire_options);
  c.written = 0;
  c.state = ConnState::Writing;
}

bool Worker::step_write(Conn& c) {
  const Wire& wire = *c.wire;
  while (c.written < wire.total()) {
    if (!c.writable) return false;
    iovec iov[kMaxIov];
    const std::size_t count = wire.fill_iovecs(iov, c.written);
    std::size_t offered = 0;
    for (std::size_t i = 0; i < count; ++i) offered += iov[i].iov_len;
    // Plain sockets take the iovecs in one call. TLS has no scatter write: OpenSSL 3.5 has
    // SSL_write_ex, but one SSL_write per iovec is simpler and the response has few of them.
    std::size_t wrote = 0;
    Io result = Io::Closed;
    if (c.ssl) {
      std::size_t at = 0;
      for (std::size_t i = 0; i < count; ++i) {
        result = io_write(c, static_cast<const char*>(iov[i].iov_base), iov[i].iov_len, wrote);
        at += wrote;
        if (result != Io::Ok || wrote < iov[i].iov_len) break;
      }
      wrote = at;
    } else {
      msghdr message{};
      message.msg_iov = iov;
      message.msg_iovlen = count;
      const ssize_t n = ::sendmsg(c.fd, &message, MSG_NOSIGNAL);
      if (n >= 0) {
        wrote = static_cast<std::size_t>(n);
        result = Io::Ok;
      } else if (errno == EINTR) {
        continue;
      } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        c.writable = false;
        return false;
      }
    }
    if (result != Io::Ok) {
      if (wrote == 0) {
        close_conn(c);
        return false;
      }
      result = Io::Ok;  // a short write: the rest goes on the next event
    }
    c.written += wrote;
    if (wrote < offered) c.writable = false;
  }
  request_done(c);
  return true;
}

void Worker::request_done(Conn& c) {
  wheel_.cancel(c.timer);
  const std::size_t consumed = c.consumed;
  release_request(c);
  c.rbuf.consume(consumed);
  if (c.close_after) {
    if (c.rbuf.empty()) {
      close_conn(c);
      return;
    }
    // Unread bytes are on the way: a close now could reset the connection and lose the reply.
    shutdown_write(c);
    c.rbuf.release();
    c.state = ConnState::Linger;
    arm(c, static_cast<std::int64_t>(kLingerMs), now_ms_, kTimerLinger);
    return;
  }
  begin_wait(c);
}

void Worker::release_request(Conn& c) {
  c.wire.reset();
  c.response.reset();
  c.ctx.reset();
  c.chunked.reset();
  c.handler = nullptr;
  c.head = ParsedHead{};
  c.body_size = 0;
  if (c.arena) give_arena(std::move(c.arena));
}

void Worker::reply_error(Conn& c, int status) {
  std::string text = "HTTP/1.1 ";
  text += std::to_string(status);
  text += ' ';
  text += reason_phrase(status);
  text += "\r\n";
  if (status == 413 && c.via_front && options_.front_headers) {
    // The front answers the oversize body: Rust handler.rs `too_large`, with its headers.
    const bool cacheable = should_cache_request(c.head.request);
    if (cacheable) {
      text += "vary: Accept-Encoding\r\nx-cache: miss\r\n";
    } else {
      text += "x-cache: bypass\r\nvary: Accept-Encoding\r\n";
    }
    char buffer[32];
    text += "date: ";
    text += http_date_now(buffer);
    text += "\r\n";
  }
  text += "connection: close\r\ncontent-length: 0\r\n\r\n";
  send_text(c, text);
  release_request(c);
  c.rbuf.release();
  shutdown_write(c);
  c.state = ConnState::Linger;
  arm(c, static_cast<std::int64_t>(kLingerMs), now_ms_, kTimerLinger);
}

bool Worker::step_linger(Conn& c) {
  char scratch[kReadStep];
  while (c.readable) {
    std::size_t got = 0;
    if (io_read(c, scratch, sizeof scratch, got) != Io::Ok) {
      if (got != 0) continue;
      close_conn(c);
      return false;
    }
  }
  return false;
}

void Worker::on_timer(TimerNode& node) {
  Conn& c = *static_cast<Conn*>(node.owner);
  if (c.closed) return;
  switch (node.kind) {
    case kTimerHead:
      // A part of a head is there: answer 408. An idle connection closes with no reply.
      if (c.state == ConnState::Head && !c.rbuf.empty()) {
        reply_error(c, 408);
        pump(c);
      } else {
        close_conn(c);
      }
      break;
    case kTimerBody:
      if (c.state == ConnState::Body) {
        reply_error(c, 408);
        pump(c);
      }
      break;
    case kTimerWrite:
    case kTimerLinger:
    default: close_conn(c); break;
    case kTimerHandshake:
      // The handshake waits for a certificate that ACME still has to get (Rust bounds the whole
      // handshake with HTTP_READ_TIMEOUT). Try again, until that timeout is over.
      if (c.state != ConnState::Handshake ||
          (options_.read_timeout_ms > 0 &&
           now_ms_ - c.accepted_ms >= static_cast<std::uint64_t>(options_.read_timeout_ms))) {
        close_conn(c);
      } else {
        pump(c);
      }
      break;
    case kTimerH2:
      // The HTTP/2 idle timeout: no stream open for that long ends the session.
      if (c.state == ConnState::Http2) h2_on_timer(c);
      break;
  }
}

void Worker::close_conn(Conn& c) {
  if (c.fd >= 0) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, c.fd, nullptr);
    ::close(c.fd);
    c.fd = -1;
    connection_count_.fetch_sub(1);
  }
  c.closed = true;
  wheel_.cancel(c.timer);
  if (c.handler_active || c.in_start || c.discarded) return;
  discard(c);
}

void Worker::abort_conn(Conn& c) {
  close_conn(c);
}

void Worker::discard(Conn& c) {
  c.discarded = true;
  release_request(c);
  c.rbuf.release();
  const std::size_t i = c.index;
  std::unique_ptr<Conn> owned = std::move(conns_[i]);
  if (i + 1 != conns_.size()) {
    conns_[i] = std::move(conns_.back());
    conns_[i]->index = i;
  }
  conns_.pop_back();
  graveyard_.push_back(std::move(owned));
}

}  // namespace campfire::net
