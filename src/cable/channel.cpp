// Subscription and ChannelRegistry. Rails: Channel::Base, Channel::Streams, Channel::Naming.
// Rust: crates/cable/src/channel.rs.
#include "cable/channel.hpp"

#include <algorithm>

#include "cable/connection.hpp"
#include "cable/protocol.hpp"

namespace campfire::cable {

std::string Subscription::channel_name() const {
  return protocol::channel_name(class_name_);
}

std::string Subscription::broadcasting_for(std::span<const std::string_view> parts) const {
  return protocol::broadcasting_for(class_name_, parts);
}

void Subscription::stream_from(std::string_view broadcasting) {
  if (unsubscribed_) return;
  GroupId group = hub_->subscribe(connection_->worker_, broadcasting, encoded_identifier_, connection_);
  streams_.push_back(Stream{std::string(broadcasting), group});
}

void Subscription::stream_for(std::span<const std::string_view> parts) {
  stream_from(broadcasting_for(parts));
}

void Subscription::stop_stream_from(std::string_view broadcasting) {
  for (std::size_t i = 0; i < streams_.size();) {
    if (streams_[i].name == broadcasting) {
      hub_->unsubscribe(connection_->worker_, streams_[i].name, streams_[i].group, connection_);
      streams_.erase(streams_.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
}

void Subscription::stop_all_streams() {
  for (auto& s : streams_) hub_->unsubscribe(connection_->worker_, s.name, s.group, connection_);
  streams_.clear();
}

void Subscription::transmit_encoded(std::string_view json) {
  connection_->queue(protocol::message(*encoded_identifier_, json));
}

void Subscription::transmit(const compat::json::Value& message) {
  transmit_encoded(compat::json::encode(message));
}

void ChannelRegistry::add(std::string class_name, ChannelFactory factory) {
  map_.insert_or_assign(std::move(class_name), std::move(factory));
}

ChannelRegistry::Found ChannelRegistry::find(std::string_view requested) const {
  if (requested.starts_with("::")) requested.remove_prefix(2);
  auto it = map_.find(std::string(requested));
  if (it == map_.end()) return {};
  return Found{&it->first, &it->second};
}

}  // namespace campfire::cable
