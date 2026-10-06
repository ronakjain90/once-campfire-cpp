// libFuzzer target for the request parser. Rust: hyper's parser (crates/kit/src/front/conn.rs).
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "core/arena.hpp"
#include "net/parser.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace campfire::net;
  const std::string text(reinterpret_cast<const char*>(data), size);
  campfire::Arena arena(1024);
  ParsedHead head;
  const ParserLimits limits;
  // The whole input at once, and in two pieces (the incremental path with `scanned`).
  const HeadResult whole = parse_head(text, 0, arena, limits, head);
  if (size > 1) {
    const std::size_t cut = 1 + (size > 2 ? data[0] % (size - 1) : 0);
    campfire::Arena second(1024);
    ParsedHead head2;
    const HeadResult first_part = parse_head(std::string_view(text).substr(0, cut), 0, second, limits, head2);
    if (first_part.status == HeadStatus::NeedMore) {
      second.reset();
      const HeadResult again = parse_head(text, cut, second, limits, head2);
      // A valid head must be found when the bytes come in two pieces. On an invalid head the two
      // paths of picohttpparser may stop at different points (Error or NeedMore).
      if (whole.status == HeadStatus::Ok && again.status != HeadStatus::Ok) std::abort();
    }
  }
  if (whole.status == HeadStatus::Ok) {
    // Touch every view of the request.
    std::size_t sum = head.request.path.size() + head.request.query.size() + head.request.target.size();
    for (const Header& h : head.request.headers) sum += h.name.size() + h.value.size();
    if (head.body_kind == BodyKind::Chunked) {
      std::string body = text.substr(head.head_size);
      ChunkedBody decoder(1 << 20);
      std::size_t have = body.size();
      (void)decoder.feed(body.data(), have);
      if (have > body.size()) std::abort();
    }
    if (sum == static_cast<std::size_t>(-1)) return 1;
  }
  return 0;
}
