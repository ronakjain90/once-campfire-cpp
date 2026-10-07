// The state of an upgraded connection. See net/ws.hpp and worker_ws.cpp.
#pragma once

#include <deque>
#include <memory>

#include "net/timer_wheel.hpp"
#include "net/ws.hpp"

namespace campfire::net {

class Worker;
struct Conn;

// The transport that the session gets, and what the worker keeps for the connection.
struct WsState final : WsTransport {
  WsState(Worker& owner, Conn& connection) : worker(&owner), conn(&connection) {}

  void send(std::span<const WsBytes> buffers) override;
  void close(std::chrono::milliseconds grace) override;

  Worker* worker;
  Conn* conn;
  std::unique_ptr<WsSession> session;
  std::deque<WsBytes> queue;     // buffers that wait for the socket
  std::size_t front_offset = 0;  // bytes of queue.front() that are written
  bool closing = false;          // `close` was called: shut the write side when the queue is empty
  bool shut = false;             // the write side is shut
  bool notified = false;         // `on_closed` ran
  TimerNode grace_timer;         // the end of the grace of `close`
};

}  // namespace campfire::net
