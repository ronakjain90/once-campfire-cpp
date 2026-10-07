// HTTP/1.1 request head and body parser. Rust: crates/kit/src/front/conn.rs (hyper); Rails: Puma.
#pragma once

#include <picohttpparser.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/arena.hpp"
#include "net/http.hpp"

namespace campfire::net {

inline constexpr std::size_t kMaxHeaders = 100;  // the limit of hyper

enum class BodyKind : std::uint8_t { None, Length, Chunked };

// The result of the parse of one request head.
struct ParsedHead {
  Request request;            // views point into the buffer, and into the arena for the header list
  std::size_t head_size = 0;  // bytes of the head, with the blank line
  BodyKind body_kind = BodyKind::None;
  std::uint64_t content_length = 0;
  bool expect_continue = false;
};

enum class HeadStatus : std::uint8_t { Ok, NeedMore, Error };

struct HeadResult {
  HeadStatus status = HeadStatus::NeedMore;
  int error_status = 0;  // for Error: the status code of the reply (400, 431, 414, 501)
};

struct ParserLimits {
  std::size_t max_head_bytes = 64 * 1024;    // 431 above this
  std::size_t max_target_bytes = 16 * 1024;  // 414 above this
};

// Parses the head at the start of `data`. `scanned` is the number of bytes that an earlier call
// already saw (0 for the first call). The arena holds the header list. The result is valid while
// the buffer and the arena are valid.
[[nodiscard]] HeadResult parse_head(std::string_view data, std::size_t scanned, Arena& arena,
                                    const ParserLimits& limits, ParsedHead& out);

// Incremental decoder of a chunked body. The decoder works in place: the decoded bytes go to the
// start of the body region, and the framing is removed.
class ChunkedBody {
 public:
  enum class Status : std::uint8_t { Done, NeedMore, Error, TooLarge };

  explicit ChunkedBody(std::uint64_t max_decoded);

  // `region` is the memory that starts at the first byte of the body. `have` is the number of
  // valid bytes in the region (decoded bytes, then new raw bytes). The decoder remembers how many
  // bytes it saw. On return, `have` is the new number of valid bytes: the decoded bytes, and
  // if Done, the bytes after the message (pipelined data) directly behind them.
  Status feed(char* region, std::size_t& have);

  [[nodiscard]] std::size_t decoded() const noexcept { return decoded_; }
  // Valid after Done: the bytes behind the decoded body.
  [[nodiscard]] std::size_t leftover() const noexcept { return leftover_; }

 private:
  phr_chunked_decoder decoder_{};
  std::uint64_t max_decoded_;
  std::size_t decoded_ = 0;
  std::size_t leftover_ = 0;
};

}  // namespace campfire::net
