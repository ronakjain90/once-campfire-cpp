// The interface of an Action Cable channel. Rails: ActionCable::Channel::Base and
// Channel::Streams. Rust: crates/cable/src/channel.rs. Channels are written in src/app/channels/.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cable/hub.hpp"
#include "compat/json.hpp"
#include "core/error.hpp"

namespace campfire::cable {

class Connection;

// The state of one subscription. A channel callback uses it to stream, to reject and to transmit.
// It lives on the worker of the connection.
class Subscription {
 public:
  // The identifier string, byte for byte as the client sent it.
  [[nodiscard]] const std::string& identifier() const { return identifier_; }
  // The decoded identifier, with "channel". An object, because only an object identifier is accepted.
  [[nodiscard]] const compat::json::Value& params() const { return params_; }
  [[nodiscard]] const compat::json::Value* param(std::string_view key) const { return params_.find(key); }
  // The value that the connection authenticator gave (identified_by :current_user).
  template <class T>
  [[nodiscard]] const T& user() const {
    return *static_cast<const T*>(user_.get());
  }
  [[nodiscard]] const std::shared_ptr<const void>& user_ptr() const { return user_; }

  [[nodiscard]] std::string channel_name() const;
  [[nodiscard]] std::string broadcasting_for(std::span<const std::string_view> parts) const;

  void stream_from(std::string_view broadcasting);
  void stream_for(std::span<const std::string_view> parts);
  // Removes each stream with this name.
  void stop_stream_from(std::string_view broadcasting);
  void stop_all_streams();
  // `stream_or_reject_for`: an empty optional rejects the subscription.
  void reject() { rejected_ = true; }
  [[nodiscard]] bool rejected() const { return rejected_; }
  [[nodiscard]] bool unsubscribed() const { return unsubscribed_; }

  // Sends {"identifier":...,"message":<json>} to this subscriber only. `transmit_encoded` takes JSON text.
  void transmit_encoded(std::string_view json);
  void transmit(const compat::json::Value& message);

  [[nodiscard]] Hub& hub() const { return *hub_; }

 private:
  friend class Connection;
  struct Stream {
    std::string name;
    GroupId group = 0;
  };
  Subscription() = default;

  Connection* connection_ = nullptr;
  Hub* hub_ = nullptr;
  std::string class_name_;
  std::string identifier_;
  EncodedIdentifier encoded_identifier_;
  compat::json::Value params_;
  std::shared_ptr<const void> user_;
  std::vector<Stream> streams_;
  bool rejected_ = false;
  bool unsubscribed_ = false;
};

// An exception that escapes a callback in Rails is logged and otherwise ignored. A callback returns
// an Error for the same case. The connection logs it and sends nothing.
class Channel {
 public:
  virtual ~Channel() = default;
  virtual Status subscribed(Subscription& sub) {
    (void)sub;
    return {};
  }
  // Also runs for a rejected subscription, as in Rails.
  virtual Status unsubscribed(Subscription& sub) {
    (void)sub;
    return {};
  }
  // `action` is data["action"], or "receive" if that is blank. Returns false if the action is not
  // a public method of the channel (Rails logs "Unable to process").
  virtual Result<bool> perform(std::string_view action, const compat::json::Value& data, Subscription& sub) {
    (void)action;
    (void)data;
    (void)sub;
    return false;
  }
};

// ApplicationCable::Channel and HeartbeatChannel: subscribe, confirm and nothing else.
class EmptyChannel final : public Channel {};

using ChannelFactory = std::function<std::unique_ptr<Channel>()>;

// The channel classes of the app, by Ruby class name ("RoomChannel", "Turbo::StreamsChannel").
class ChannelRegistry {
 public:
  void add(std::string class_name, ChannelFactory factory);
  struct Found {
    const std::string* class_name = nullptr;
    const ChannelFactory* factory = nullptr;
  };
  // safe_constantize also resolves "::RoomChannel". The class name stays "RoomChannel".
  [[nodiscard]] Found find(std::string_view requested) const;

 private:
  std::unordered_map<std::string, ChannelFactory> map_;
};

}  // namespace campfire::cable
