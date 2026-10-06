// libFuzzer target of the Action Cable commands: a connection that reads arbitrary text messages.
// Rust: crates/cable/src/connection.rs.
#include <cstddef>
#include <cstdint>
#include <string>

#include "cable/connection.hpp"

namespace {
struct NullTransport final : campfire::cable::Transport {
  void send(std::span<const campfire::cable::ws::Bytes>) override {}
  void close(std::chrono::milliseconds) override {}
};
struct Heartbeat final : campfire::cable::Channel {
  campfire::Result<bool> perform(std::string_view, const campfire::compat::json::Value&,
                                 campfire::cable::Subscription& sub) override {
    std::string_view parts[] = {"a"};
    sub.stream_for(parts);
    sub.transmit_encoded("{}");
    return true;
  }
};
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace campfire::cable;
  static ChannelRegistry registry = [] {
    ChannelRegistry r;
    r.add("HeartbeatChannel", [] { return std::make_unique<Heartbeat>(); });
    return r;
  }();
  Hub hub(1, nullptr);
  NullTransport transport;
  Connection conn(hub, 0, transport, registry, false);
  conn.open(nullptr, "user-1");
  // Each line is one text message, so the fuzzer reaches the command parser quickly.
  std::string_view all(reinterpret_cast<const char*>(data), size);
  while (!all.empty()) {
    auto nl = all.find('\n');
    std::string_view line = all.substr(0, nl);
    std::string payload(line);
    std::uint8_t head[10];
    std::size_t n = ws::encode_header(head, ws::Opcode::Text, false, payload.size());
    head[1] |= 0x80;
    std::string frame(reinterpret_cast<char*>(head), n);
    frame.append(4, '\0');  // a zero mask
    frame += payload;
    conn.on_data(frame);
    hub.drain(0);
    if (nl == std::string_view::npos) break;
    all.remove_prefix(nl + 1);
  }
  return 0;
}
