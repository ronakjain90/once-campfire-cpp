// A text frame that many connections share. Rust: crates/cable/src/socket.rs (Frame).
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "cable/websocket.hpp"

namespace campfire::cable {

// The wire bytes of one server text frame. The plain bytes are built at once. The deflated bytes
// are built the first time that a connection with permessage-deflate needs them, then shared.
// A frame is immutable after make(), so any thread can read it.
class Frame {
 public:
  [[nodiscard]] static std::shared_ptr<const Frame> make(std::string text);

  // The payload (JSON text).
  [[nodiscard]] std::string_view text() const { return std::string_view(*plain_).substr(header_size_); }
  // The bytes for a connection. Short frames are never compressed.
  [[nodiscard]] ws::Bytes wire(bool deflate) const;

 private:
  Frame() = default;
  ws::Bytes plain_;
  std::size_t header_size_ = 0;
  mutable std::once_flag once_;
  mutable ws::Bytes deflated_;
};

using FramePtr = std::shared_ptr<const Frame>;

}  // namespace campfire::cable
