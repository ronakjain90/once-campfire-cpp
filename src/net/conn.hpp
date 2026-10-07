// Connection state of the HTTP/1.1 server. Rust: crates/kit/src/front/conn.rs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "core/arena.hpp"
#include "core/task.hpp"
#include "net/ctx.hpp"
#include "net/front/front.hpp"
#include "net/front/tls.hpp"
#include "net/parser.hpp"
#include "net/response.hpp"
#include "net/timer_wheel.hpp"
#include "net/ws.hpp"

namespace campfire::net {

// The bytes read from the socket and not yet used. The buffer keeps its memory only while it
// holds data, so an idle connection costs no buffer.
class ReadBuffer {
 public:
  static constexpr std::size_t kInitial = 8192;

  [[nodiscard]] char* data() noexcept { return data_.get() + begin_; }
  [[nodiscard]] const char* data() const noexcept { return data_.get() + begin_; }
  [[nodiscard]] std::size_t size() const noexcept { return end_ - begin_; }
  [[nodiscard]] bool empty() const noexcept { return end_ == begin_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  // Free room behind the data.
  [[nodiscard]] std::size_t room() const noexcept { return capacity_ - end_; }

  // Makes sure that `total` bytes of data fit (counted from the start of the data).
  void reserve(std::size_t total);
  [[nodiscard]] char* tail() noexcept { return data_.get() + end_; }
  void commit(std::size_t n) noexcept { end_ += n; }
  // Sets the size of the data (the chunked decoder shrinks it).
  void resize(std::size_t n) noexcept { end_ = begin_ + n; }
  // Drops the first `n` bytes. If the buffer is empty after this, it gives its memory back.
  void consume(std::size_t n) noexcept;
  void release() noexcept;

 private:
  std::unique_ptr<char[]> data_;
  std::size_t capacity_ = 0;
  std::size_t begin_ = 0;
  std::size_t end_ = 0;
};

enum class ConnState : std::uint8_t {
  Head,       // waiting for a request head (idle, or part of a head)
  Body,       // the head is parsed, the body is not complete
  Handling,   // a coroutine runs the handler
  Writing,    // the response is in the send buffer or on the way
  Linger,     // the reply is written and the write side is shut: drain reads, then close
  Handshake,  // the TLS handshake runs (A8)
  Http2,      // an HTTP/2 session runs on the connection (A8)
  WebSocket,  // a 101 reply is written: a `WsSession` owns the bytes
};

enum TimerKind : std::uint32_t {
  kTimerHead = 1,
  kTimerBody,
  kTimerWrite,
  kTimerLinger,
  kTimerHandshake,
  kTimerH2,
  kTimerWsStall,
  kTimerWsClose,
  kTimerBeat
};

class Worker;
class H2Session;
struct WsState;

struct Conn {
  Conn();
  ~Conn();
  Conn(const Conn&) = delete;
  Conn& operator=(const Conn&) = delete;

  int fd = -1;
  bool via_front = false;
  bool tls = false;  // the connection is TLS (A8)
  front::SslPtr ssl;
  bool redirect_mode = false;  // TLS is on and this is the HTTP port: answer HTTP-01 and redirect
  bool h2c = false;            // H2C_ENABLED: this plain connection may speak HTTP/2 (prior knowledge)
  std::uint64_t accepted_ms = 0;
  std::unique_ptr<H2Session> h2;
  std::unique_ptr<WsState> ws;  // the state of an upgraded connection
  bool readable = false;        // edge-triggered: the socket may hold data
  bool writable = true;         // edge-triggered: the socket may take data
  bool closed = false;          // the descriptor is closed; the object waits for the end of the handler
  bool handler_active = false;
  bool close_after = false;  // close when the response is written
  bool continue_sent = false;
  bool discarded = false;
  bool in_start = false;  // the handler is in its first run, inside `dispatch`
  std::size_t index = 0;  // position in the table of the worker
  ConnState state = ConnState::Head;
  TimerNode timer;
  char remote[48] = {};
  std::uint8_t remote_size = 0;

  ReadBuffer rbuf;
  std::size_t scanned = 0;  // bytes of a partial head that the parser already saw
  std::uint64_t request_start_ms = 0;

  // The current request.
  std::unique_ptr<Arena> arena;
  ParsedHead head;
  std::optional<ChunkedBody> chunked;
  std::size_t body_size = 0;  // bytes of the decoded body, after the head, in `rbuf`
  HandlerFn handler = nullptr;
  std::optional<Ctx> ctx;
  Task<void> task;
  std::optional<Response> response;
  front::FrontState front_state;
  std::optional<Wire> wire;
  std::size_t written = 0;
  std::size_t consumed = 0;  // bytes of `rbuf` that belong to the current request
  int pending_error = 0;     // a status for a reply made by the server, with no handler
};

}  // namespace campfire::net
