// WebSocket connections of a worker: the hand-off after the 101 reply, reads, queued writes, the stall and close
// timers. Rails: ActionCable::Connection::ClientSocket, Stream. Rust: crates/cable/src/connection.rs, socket.rs.
#include <sys/socket.h>
#include <sys/uio.h>

#include <algorithm>
#include <cerrno>
#include <string>

#include "core/log.hpp"
#include "net/worker.hpp"
#include "net/ws_state.hpp"

namespace campfire::net {

namespace {

constexpr std::size_t kReadStep = 4096;  // read buffers are 4 KiB (architecture section 10)
constexpr std::size_t kMaxIov = 32;

}  // namespace

void WsState::send(std::span<const WsBytes> buffers) {
  if (conn->closed || shut) return;
  for (const WsBytes& buffer : buffers) {
    if (buffer != nullptr && !buffer->empty()) queue.push_back(buffer);
  }
  worker->ws_flush(*conn);
  // The client does not read: the queue grows past the limit.
  if (!conn->closed && queue.size() > kWsMaxQueuedBuffers && session != nullptr) {
    queue.clear();
    front_offset = 0;
    session->on_lagged();
  }
}

void WsState::close(std::chrono::milliseconds grace) {
  if (conn->closed) return;
  closing = true;
  if (grace.count() <= 0) {
    // After a write stall the queue cannot move: close in any case.
    worker->close_conn(*conn);
    return;
  }
  worker->wheel_.arm(grace_timer, worker->now_ms_, static_cast<std::uint64_t>(grace.count()), kTimerWsClose);
  worker->ws_flush(*conn);
}

Scheduler& WsState::scheduler() noexcept {
  return *worker;
}

void Worker::ws_start(Conn& c, Response& response) {
  // The 101 reply is the first bytes that the connection writes. The server did not read past the request: what is
  // left in the read buffer belongs to the session.
  WsAccept accept = std::move(response.ws_accept);
  WireOptions wire_options;
  wire_options.http_minor = c.head.request.minor_version;
  const Wire wire(c.arena->resource(), response, wire_options);
  std::string head;
  head.reserve(wire.total());
  iovec iov[kMaxIov];
  std::size_t at = 0;
  while (at < wire.total()) {
    const std::size_t count = wire.fill_iovecs(iov, at);
    for (std::size_t i = 0; i < count; ++i) {
      head.append(static_cast<const char*>(iov[i].iov_base), iov[i].iov_len);
      at += iov[i].iov_len;
    }
  }
  const std::size_t consumed = c.consumed;
  wheel_.cancel(c.timer);
  c.wire.reset();
  release_request(c);
  c.rbuf.consume(consumed);
  c.close_after = false;
  c.state = ConnState::WebSocket;
  c.ws = std::make_unique<WsState>(*this, c);
  c.ws->grace_timer.owner = &c;
  c.ws->queue.push_back(std::make_shared<const std::string>(std::move(head)));
  c.ws->session = accept(*c.ws, index_);
  if (c.closed) return;
  if (!c.ws->session) {
    close_conn(c);
    return;
  }
  ws_flush(c);
  // Bytes that came with the request (a client that does not wait for the reply).
  while (!c.closed && !c.rbuf.empty()) {
    const std::size_t n = std::min(c.rbuf.size(), kReadStep);
    const std::string chunk(c.rbuf.data(), n);
    c.rbuf.consume(n);
    c.ws->session->on_data(chunk);
  }
  if (!c.closed) ws_flush(c);
}

bool Worker::ws_step(Conn& c) {
  WsState& ws = *c.ws;
  ws_flush(c);
  if (c.closed) return false;
  char buffer[kReadStep];
  while (c.readable && !c.closed) {
    std::size_t got = 0;
    const Io result = io_read(c, buffer, sizeof buffer, got);
    if (got != 0) {
      ws.session->on_data(std::string_view(buffer, got));
      if (c.closed) return false;
    }
    if (result == Io::WouldBlock) {
      c.readable = false;
    } else if (result == Io::Closed) {
      // The peer ended (also its answer to our close frame): the connection is over.
      close_conn(c);
      return false;
    } else if (got < sizeof buffer && !c.ssl) {
      c.readable = false;  // a short read of a plain socket: nothing more is there
    }
  }
  ws_flush(c);
  return false;
}

void Worker::ws_flush(Conn& c) {
  WsState& ws = *c.ws;
  if (c.closed) return;
  bool progress = false;
  while (!ws.queue.empty()) {
    if (!c.writable) break;
    iovec iov[kMaxIov];
    std::size_t count = 0;
    std::size_t offered = 0;
    std::size_t offset = ws.front_offset;
    for (const WsBytes& buffer : ws.queue) {
      if (count == kMaxIov) break;
      iov[count] = {const_cast<char*>(buffer->data()) + offset, buffer->size() - offset};
      offered += iov[count].iov_len;
      ++count;
      offset = 0;
    }
    std::size_t wrote = 0;
    if (c.ssl) {
      // TLS has no scatter write: one write for each buffer.
      for (std::size_t i = 0; i < count; ++i) {
        std::size_t one = 0;
        const Io result = io_write(c, static_cast<const char*>(iov[i].iov_base), iov[i].iov_len, one);
        wrote += one;
        if (result == Io::Closed && one == 0 && wrote == 0) {
          close_conn(c);
          return;
        }
        if (result != Io::Ok || one < iov[i].iov_len) break;
      }
    } else {
      msghdr message{};
      message.msg_iov = iov;
      message.msg_iovlen = count;
      const ssize_t n = ::sendmsg(c.fd, &message, MSG_NOSIGNAL);
      if (n >= 0) {
        wrote = static_cast<std::size_t>(n);
      } else if (errno == EINTR) {
        continue;
      } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        c.writable = false;
        break;
      } else {
        close_conn(c);
        return;
      }
    }
    if (wrote == 0) {
      c.writable = false;
      break;
    }
    progress = true;
    std::size_t left = wrote;
    while (left != 0 && !ws.queue.empty()) {
      const std::size_t remaining = ws.queue.front()->size() - ws.front_offset;
      if (left >= remaining) {
        left -= remaining;
        ws.queue.pop_front();
        ws.front_offset = 0;
      } else {
        ws.front_offset += left;
        left = 0;
      }
    }
    if (wrote < offered) c.writable = false;
  }
  if (ws.queue.empty()) {
    wheel_.cancel(c.timer);
    if (ws.closing && !ws.shut) {
      ws.shut = true;
      shutdown_write(c);
    }
  } else if (progress || !c.timer.armed) {
    // The write stall: the queue did not move for this long.
    arm(c, static_cast<std::int64_t>(kWsWriteStallSeconds) * 1000, now_ms_, kTimerWsStall);
  }
}

void Worker::ws_notify_closed(Conn& c) {
  WsState& ws = *c.ws;
  wheel_.cancel(ws.grace_timer);
  if (ws.notified) return;
  ws.notified = true;
  ws.queue.clear();
  if (ws.session) ws.session->on_closed();
}

void Worker::ws_on_timer(Conn& c, TimerNode& node) {
  if (!c.ws) return;
  if (node.kind == kTimerWsClose) {
    close_conn(c);
    return;
  }
  // kTimerWsStall: the queued bytes did not move.
  if (!c.ws->queue.empty() && c.ws->session) {
    c.ws->session->on_write_stall();
  }
}

}  // namespace campfire::net
