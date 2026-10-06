// The connection state machine. Rails: ActionCable::Connection::Base, Connection::Subscriptions,
// InternalChannel. Rust: crates/cable/src/connection.rs.
#include "cable/connection.hpp"

#include <algorithm>
#include <cctype>

#include "core/log.hpp"

namespace campfire::cable {

namespace json = compat::json;

namespace {
bool blank(const std::string& s) {
  return std::all_of(s.begin(), s.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
}
}  // namespace

Connection::Connection(Hub& hub, unsigned worker, Transport& transport, const ChannelRegistry& channels, bool deflate)
    : hub_(hub),
      worker_(worker),
      transport_(transport),
      channels_(channels),
      deflate_(deflate),
      parser_(ws::ParserOptions{.deflate = deflate}),
      internal_(*this) {}

Connection::~Connection() { on_closed(); }

void Connection::open(std::shared_ptr<const void> user, std::string_view connection_identifier,
                      const std::function<bool()>& recheck) {
  user_ = std::move(user);
  if (!connection_identifier.empty()) {
    internal_stream_ = protocol::internal_channel(connection_identifier);
    internal_group_ = hub_.subscribe(worker_, internal_stream_, nullptr, &internal_);
    if (recheck && !recheck()) {
      hub_.unsubscribe(worker_, internal_stream_, internal_group_, &internal_);
      internal_group_ = 0;
      reject_unauthorized();
      return;
    }
  }
  hub_.attach(worker_, this);
  attached_ = true;
  queue(protocol::welcome());
  flush();
}

void Connection::reject_unauthorized() {
  log_error("An unauthorized connection attempt was rejected");
  disconnect(protocol::DisconnectReason::Unauthorized, "false");
}

void Connection::on_data(std::string_view bytes) {
  if (closed_) return;
  parser_.feed(bytes);
  while (!closed_) {
    auto next = parser_.next();
    if (!next) {
      write_raw(ws::encode_close(next.error()));
      transport_.close(std::chrono::milliseconds(0));
      closed_ = true;
      return;
    }
    if (!*next) break;
    handle_message(**next);
  }
  flush();
}

void Connection::handle_message(ws::Message& message) {
  using Type = ws::Message::Type;
  if (closing_) {
    // After our close frame, only the answer of the client matters.
    if (message.type == Type::Close) transport_.close(std::chrono::milliseconds(0));
    return;
  }
  switch (message.type) {
    case Type::Text: dispatch(message.data); break;
    case Type::Binary: log_error("Couldn't handle non-string message: Array"); break;
    case Type::Ping: write_raw(ws::encode_frame(ws::Opcode::Pong, false, message.data)); break;
    case Type::Pong: break;
    case Type::Close:
      flush();
      write_raw(ws::encode_close_reply(message.close_code));
      transport_.close(std::chrono::milliseconds(0));
      closing_ = true;
      break;
  }
}

void Connection::on_write_stall() {
  log_error("Closing a connection: no write progress for {} seconds", protocol::kWriteStallSeconds);
  closing_ = true;
  transport_.close(std::chrono::milliseconds(0));
}

void Connection::on_lagged() {
  pending_.clear();  // Connection::Base#close without a reason: the client reconnects.
  disconnect(std::nullopt, "true");
}

void Connection::on_closed() {
  if (cleaned_) return;
  cleaned_ = true;
  closed_ = true;
  closing_ = true;
  while (!entries_.empty()) remove_entry(0);
  pending_.clear();
  if (internal_group_ != 0) {
    hub_.unsubscribe(worker_, internal_stream_, internal_group_, &internal_);
    internal_group_ = 0;
  }
  if (attached_) {
    hub_.detach(worker_, this);
    attached_ = false;
  }
}

void Connection::flush() {
  if (pending_.empty()) return;
  if (closing_ || closed_) {
    pending_.clear();
    return;
  }
  std::vector<ws::Bytes> buffers;
  buffers.reserve(pending_.size());
  for (const auto& f : pending_) buffers.push_back(f->wire(deflate_));
  pending_.clear();
  transport_.send(buffers);
}

void Connection::write_raw(const std::string& bytes) {
  ws::Bytes b = std::make_shared<const std::string>(bytes);
  transport_.send(std::span<const ws::Bytes>(&b, 1));
}

void Connection::disconnect(std::optional<protocol::DisconnectReason> reason, std::string_view reconnect_json) {
  flush();
  if (closing_ || closed_) return;
  FramePtr frame = Frame::make(protocol::disconnect(reason, reconnect_json));
  ws::Bytes buffers[2] = {frame->wire(deflate_), std::make_shared<const std::string>(ws::encode_close(ws::kCloseNormal))};
  transport_.send(buffers);
  closing_ = true;
  transport_.close(kCloseGrace);
}

void Connection::on_restart() {
  disconnect(protocol::DisconnectReason::ServerRestart, "true");
}

// InternalChannel#process_internal_message: only {"type":"disconnect"} counts.
void Connection::on_internal(std::string_view payload) {
  auto message = json::parse(payload);
  if (!message || !message->is_object()) return;
  const json::Value* type = message->find("type");
  if (type == nullptr || !type->is_string() || type->as_string() != "disconnect") return;
  const json::Value* reconnect = message->find("reconnect");
  disconnect(protocol::DisconnectReason::Remote, reconnect != nullptr ? json::encode(*reconnect) : "true");
}

// Subscriptions#execute_command. Anything malformed raises in Rails. Rails logs it and the
// connection stays open.
void Connection::dispatch(std::string_view text) {
  auto data = json::parse(text);
  if (!data || !data->is_object()) {
    log_error("Could not execute command from ({})", text);
    return;
  }
  const json::Value* command = data->find("command");
  std::string_view name = (command != nullptr && command->is_string()) ? std::string_view(command->as_string()) : "";
  if (name == "subscribe") {
    add(*data);
  } else if (name == "unsubscribe") {
    remove(*data);
  } else if (name == "message") {
    perform_action(*data);
  } else {
    log_error("Received unrecognized command in {}", text);
  }
}

Connection::Entry* Connection::find_identifier(std::string_view identifier, std::size_t* index) {
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i]->sub.identifier_ == identifier) {
      if (index != nullptr) *index = i;
      return entries_[i].get();
    }
  }
  return nullptr;
}

Connection::Entry* Connection::find(const json::Value& data, std::size_t* index) {
  const json::Value* id = data.find("identifier");
  if (id == nullptr || !id->is_string()) return nullptr;
  return find_identifier(id->as_string(), index);
}

// Subscriptions#add. A repeated identifier (byte for byte) gets no reply.
void Connection::add(const json::Value& data) {
  const json::Value* id = data.find("identifier");
  if (id == nullptr || !id->is_string()) {
    log_error("Could not execute command: missing identifier");
    return;
  }
  const std::string& identifier = id->as_string();
  auto params = json::parse(identifier);
  if (!params || !params->is_object()) {
    log_error("Could not execute command: invalid identifier");
    return;
  }
  if (find_identifier(identifier) != nullptr) return;
  // Limits that Rails does not have: they bound what one socket can make the server hold.
  if (entries_.size() >= protocol::kMaxSubscriptions || identifier.size() > protocol::kMaxIdentifierBytes) {
    log_error("Could not execute command: subscription limit reached ({})", entries_.size());
    return;
  }
  const json::Value* channel = params->find("channel");
  std::string_view requested = (channel != nullptr && channel->is_string()) ? std::string_view(channel->as_string()) : "";
  auto found = channels_.find(requested);
  if (found.factory == nullptr) {
    log_error("Subscription class not found: {}", requested);
    return;
  }
  auto entry = std::make_unique<Entry>();
  entry->channel = (*found.factory)();
  Subscription& sub = entry->sub;
  sub.connection_ = this;
  sub.hub_ = &hub_;
  sub.class_name_ = *found.class_name;
  sub.identifier_ = identifier;
  sub.encoded_identifier_ = std::make_shared<const std::string>(json::encode(json::Value(identifier)));
  sub.params_ = std::move(*params);
  sub.user_ = user_;
  Entry* raw = entry.get();
  entries_.push_back(std::move(entry));

  // Channel::Base#subscribe_to_channel
  Status result = raw->channel->subscribed(raw->sub);
  if (!result) {
    log_error("Could not execute command: {}", result.error().message);
    return;
  }
  if (raw->sub.rejected_) {
    std::size_t index = 0;
    if (find_identifier(identifier, &index) != nullptr) remove_entry(index);
    queue(protocol::rejection(identifier));
  } else {
    queue(protocol::confirmation(identifier));
  }
}

void Connection::remove(const json::Value& data) {
  std::size_t index = 0;
  if (find(data, &index) == nullptr) {
    log_error("Unable to find subscription with identifier");
    return;
  }
  remove_entry(index);
}

// Subscriptions#remove_subscription -> Channel::Base#unsubscribe_from_channel.
void Connection::remove_entry(std::size_t index) {
  std::unique_ptr<Entry> entry = std::move(entries_[index]);
  entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
  entry->sub.unsubscribed_ = true;
  Status result = entry->channel->unsubscribed(entry->sub);
  if (!result) log_error("Could not execute command: {}", result.error().message);
  entry->sub.stop_all_streams();
}

// Subscriptions#perform_action -> Channel::Base#perform_action.
void Connection::perform_action(const json::Value& data) {
  Entry* entry = find(data);
  if (entry == nullptr) {
    log_error("Unable to find subscription with identifier");
    return;
  }
  const json::Value* raw = data.find("data");
  std::optional<json::Value> payload;
  if (raw != nullptr && raw->is_string()) payload = json::parse(raw->as_string());
  if (!payload || !payload->is_object()) {
    log_error("Could not execute command: invalid data");
    return;
  }
  std::string action = "receive";
  if (const json::Value* a = payload->find("action"); a != nullptr && !a->is_null()) {
    if (!a->is_string()) {
      log_error("Could not execute command: invalid action");
      return;
    }
    if (!blank(a->as_string())) action = a->as_string();
  }
  Result<bool> result = entry->channel->perform(action, *payload, entry->sub);
  if (!result) {
    log_error("Could not execute command: {}", result.error().message);
  } else if (!*result) {
    log_error("Unable to process {}#{}", entry->sub.class_name_, action);
  }
}

}  // namespace campfire::cable
