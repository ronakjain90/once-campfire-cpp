// Helpers of the cable tests: a masked client frame, a decoder of server frames, a mock transport.
#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "cable/connection.hpp"
#include "cable/websocket.hpp"

namespace campfire::cable::testing {

// A client frame (masked). `fin` and `rsv1` are set by hand so that a test can fragment.
inline std::string client_frame(std::uint8_t opcode, bool fin, bool rsv1, std::string_view payload) {
  const std::uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
  std::string p(payload);
  ws::apply_mask(reinterpret_cast<std::uint8_t*>(p.data()), p.size(), mask);
  std::uint8_t head[10];
  std::size_t n = ws::encode_header(head, static_cast<ws::Opcode>(opcode), rsv1, payload.size());
  head[0] = static_cast<std::uint8_t>((head[0] & 0x7f) | (fin ? 0x80 : 0));
  head[1] |= 0x80;
  std::string out(reinterpret_cast<char*>(head), n);
  out.append(reinterpret_cast<const char*>(mask), 4);
  return out + p;
}

inline std::string text_frame(std::string_view payload) { return client_frame(1, true, false, payload); }

// What a client sees: "T:<text>" for a text frame, "C:<code>" for a close frame, "P:<payload>"
// for a pong.
inline std::vector<std::string> decode_server(std::string_view bytes) {
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (pos + 2 <= bytes.size()) {
    auto b0 = static_cast<std::uint8_t>(bytes[pos]);
    auto b1 = static_cast<std::uint8_t>(bytes[pos + 1]);
    std::size_t len = b1 & 0x7f;
    pos += 2;
    if (len == 126) {
      len = (static_cast<std::size_t>(static_cast<std::uint8_t>(bytes[pos])) << 8) | static_cast<std::uint8_t>(bytes[pos + 1]);
      pos += 2;
    } else if (len == 127) {
      len = 0;
      for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<std::uint8_t>(bytes[pos + static_cast<std::size_t>(i)]);
      pos += 8;
    }
    std::string payload(bytes.substr(pos, len));
    pos += len;
    std::uint8_t opcode = b0 & 0x0f;
    if (opcode == 1) {
      if ((b0 & 0x40) != 0) {
        auto text = ws::inflate_message(payload);
        out.push_back("T:" + (text ? *text : std::string("<bad deflate>")));
      } else {
        out.push_back("T:" + payload);
      }
    } else if (opcode == 8) {
      std::uint16_t code = payload.size() >= 2 ? static_cast<std::uint16_t>((static_cast<std::uint8_t>(payload[0]) << 8) |
                                                                           static_cast<std::uint8_t>(payload[1]))
                                               : 0;
      out.push_back("C:" + std::to_string(code));
    } else if (opcode == 10) {
      out.push_back("P:" + payload);
    }
  }
  return out;
}

// Records what the connection writes.
class MockTransport final : public Transport {
 public:
  void send(std::span<const ws::Bytes> buffers) override {
    ++sends;
    for (const auto& b : buffers) wire += *b;
  }
  void close(std::chrono::milliseconds grace) override {
    ++closes;
    last_grace = grace;
  }
  // The frames since the last take(), decoded.
  std::vector<std::string> take() {
    auto frames = decode_server(wire);
    wire.clear();
    return frames;
  }

  std::string wire;
  int sends = 0;
  int closes = 0;
  std::chrono::milliseconds last_grace{-1};
};

}  // namespace campfire::cable::testing
