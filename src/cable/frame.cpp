// Shared frame. Rust: crates/cable/src/socket.rs (Frame).
#include "cable/frame.hpp"

namespace campfire::cable {

FramePtr Frame::make(std::string text) {
  std::shared_ptr<Frame> f(new Frame());
  std::uint8_t head[10];
  f->header_size_ = ws::encode_header(head, ws::Opcode::Text, false, text.size());
  std::string wire;
  wire.reserve(f->header_size_ + text.size());
  wire.append(reinterpret_cast<const char*>(head), f->header_size_);
  wire += text;
  f->plain_ = std::make_shared<const std::string>(std::move(wire));
  return f;
}

ws::Bytes Frame::wire(bool deflate) const {
  if (!deflate || text().size() < ws::kMinCompressed) return plain_;
  std::call_once(once_, [this] {
    deflated_ = std::make_shared<const std::string>(ws::encode_frame(ws::Opcode::Text, true, ws::deflate_message(text())));
  });
  return deflated_;
}

}  // namespace campfire::cable
