// One Action Cable connection: the protocol state machine on top of a WebSocket.
// Rails: ActionCable::Connection::Base, Connection::Subscriptions, InternalChannel.
// Rust: crates/cable/src/connection.rs. It has no socket code. The transport (src/net) gives the
// bytes that it reads and takes the bytes to write.
//
// Thread rule: every member function runs on the worker thread of the connection.
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cable/channel.hpp"
#include "cable/frame.hpp"
#include "cable/hub.hpp"
#include "cable/protocol.hpp"
#include "cable/websocket.hpp"

namespace campfire::cable {

// The socket side of a connection. T5 gives the implementation.
class Transport {
 public:
  virtual ~Transport() = default;
  // Writes the buffers in order (one writev if the socket takes it). A buffer is shared.
  virtual void send(std::span<const ws::Bytes> buffers) = 0;
  // Closes the TCP connection after the pending bytes are written. If the peer closes first, the
  // transport closes at once. After `grace`, the transport closes in any case.
  virtual void close(std::chrono::milliseconds grace) = 0;
};

// The time that a connection waits for the close reply of the client (Rust: close_timeout).
inline constexpr std::chrono::milliseconds kCloseGrace{5000};

class Connection final : private Sink, private Peer {
 public:
  Connection(Hub& hub, unsigned worker, Transport& transport, const ChannelRegistry& channels, bool deflate);
  ~Connection() override;
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  // handle_open for an authenticated connection: subscribes to the internal channel (if the
  // identifier is not empty), then sends the welcome frame. `recheck` runs after the
  // subscription. If it returns false, the connection is rejected as unauthorized (this closes the
  // window in which a ban could pass unheard).
  void open(std::shared_ptr<const void> user, std::string_view connection_identifier,
            const std::function<bool()>& recheck = {});
  // Connection::Base#respond_to_invalid_request: disconnect (unauthorized, no reconnect) and close.
  void reject_unauthorized();

  // Bytes that the transport read from the socket.
  void on_data(std::string_view bytes);
  // The transport detected that a write did not progress for kWriteStallSeconds.
  void on_write_stall();
  // The transport has more unsent bytes than it allows (the client does not read). Rust: a stream
  // that lags. The connection sends {"reason":null,"reconnect":true} and closes.
  void on_lagged();
  // The transport closed. This runs handle_close: it unsubscribes every channel.
  void on_closed();

  // Writes the queued frames. The hub calls it after a batch of deliveries.
  void flush() override;

  [[nodiscard]] std::size_t subscription_count() const { return entries_.size(); }
  [[nodiscard]] bool closing() const { return closing_; }

 private:
  friend class Subscription;
  struct Entry {
    std::unique_ptr<Channel> channel;
    Subscription sub;
  };
  class Internal final : public Sink {
   public:
    explicit Internal(Connection& owner) : owner_(owner) {}
    void deliver(const FramePtr& frame) override { owner_.on_internal(frame->text()); }

   private:
    Connection& owner_;
  };

  // Sink and Peer
  void deliver(const FramePtr& frame) override { pending_.push_back(frame); }
  void on_beat(const FramePtr& ping) override {
    pending_.push_back(ping);
    flush();
  }
  void on_restart() override;

  void on_internal(std::string_view payload);
  void handle_message(ws::Message& message);
  void dispatch(std::string_view text);
  void add(const compat::json::Value& data);
  void remove(const compat::json::Value& data);
  void perform_action(const compat::json::Value& data);
  void remove_entry(std::size_t index);
  [[nodiscard]] Entry* find(const compat::json::Value& data, std::size_t* index = nullptr);
  [[nodiscard]] Entry* find_identifier(std::string_view identifier, std::size_t* index = nullptr);

  // Sends the disconnect frame and the close frame, then waits for the client.
  void disconnect(std::optional<protocol::DisconnectReason> reason, std::string_view reconnect_json);
  void write_raw(const std::string& bytes);
  void queue(std::string_view text) { pending_.push_back(Frame::make(text)); }

  Hub& hub_;
  unsigned worker_;
  Transport& transport_;
  const ChannelRegistry& channels_;
  bool deflate_;
  ws::FrameParser parser_;
  std::shared_ptr<const void> user_;
  std::vector<std::unique_ptr<Entry>> entries_;
  std::vector<FramePtr> pending_;
  Internal internal_;
  GroupId internal_group_ = 0;
  std::string internal_stream_;
  bool attached_ = false;
  bool closing_ = false;
  bool closed_ = false;
  bool cleaned_ = false;
};

}  // namespace campfire::cable
