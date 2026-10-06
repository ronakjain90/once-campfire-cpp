// libFuzzer target of the WebSocket frame parser. Rust: crates/cable/src/socket.rs.
// The first byte picks the options and the chunk size. The rest is the byte stream.
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "cable/websocket.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) return 0;
  campfire::cable::ws::ParserOptions options;
  options.deflate = (data[0] & 1) != 0;
  options.require_mask = (data[0] & 2) == 0;
  options.max_message = (data[0] & 4) != 0 ? 4096 : campfire::cable::ws::kMaxMessage;
  std::size_t chunk = std::size_t{1} + (data[0] >> 3);
  campfire::cable::ws::FrameParser parser(options);
  std::string_view rest(reinterpret_cast<const char*>(data + 1), size - 1);
  while (true) {
    std::string_view part = rest.substr(0, chunk);
    rest.remove_prefix(part.size());
    parser.feed(part);
    while (true) {
      auto next = parser.next();
      if (!next || !*next) break;
    }
    if (rest.empty()) break;
  }
  return 0;
}
