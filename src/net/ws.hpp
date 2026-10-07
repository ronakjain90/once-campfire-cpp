// The WebSocket hand-off between the HTTP server and an application protocol. Rails: ActionCable::Connection::
// ClientSocket (the socket that the server hijacks). Rust: crates/cable/src/server.rs (`on_upgrade`).
//
// A handler answers an upgrade request with a 101 response that carries a `WsAccept`. The worker writes the
// response, then it calls the function on its own thread, with the transport of the connection. From then on the
// worker gives the bytes that it reads to the session, and writes the buffers that the session sends.
//
// Thread rule: every function here runs on the worker thread of the connection.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace campfire::net {

// A buffer that a session sends. Many connections share one buffer for a broadcast.
using WsBytes = std::shared_ptr<const std::string>;

// The socket side of an upgraded connection.
class WsTransport {
 public:
  virtual ~WsTransport() = default;
  // Writes the buffers in order (one writev if the socket takes it). Whatever the socket does not take stays queued.
  virtual void send(std::span<const WsBytes> buffers) = 0;
  // Closes the TCP connection after the queued bytes are written. If the peer closes first, the transport closes at
  // once. After `grace`, the transport closes in any case.
  virtual void close(std::chrono::milliseconds grace) = 0;
};

// The application side. The worker owns the session until the connection ends.
class WsSession {
 public:
  virtual ~WsSession() = default;
  // Bytes read from the socket (4 KiB at most for each call).
  virtual void on_data(std::string_view bytes) = 0;
  // The queued bytes did not move for `kWsWriteStallSeconds`.
  virtual void on_write_stall() = 0;
  // More frames are queued than the limit: the client does not read.
  virtual void on_lagged() = 0;
  // The connection ended (the peer closed, an error, or the server stops). Runs once, before the destructor.
  virtual void on_closed() = 0;
};

// Makes the session. `worker` is the index of the worker: the hub of the app keeps one queue for each worker.
using WsAccept = std::function<std::unique_ptr<WsSession>(WsTransport& transport, unsigned worker)>;

inline constexpr unsigned kWsWriteStallSeconds = 30;
// The most buffers that may wait for the socket (Rust: `stream_capacity`, 256 messages for each stream).
inline constexpr std::size_t kWsMaxQueuedBuffers = 256;

}  // namespace campfire::net
