// Gzip and ETags for pages made mostly of message fragments, without compressing or hashing the whole page for each
// request. Rust: crates/kit/src/deflater/splice.rs.
//
// A page is split into parts at its fragments: each fragment (with the few bytes of text before it, when that text
// follows another fragment) and the text between them. Each part is compressed one time, with the part before it as a
// preset dictionary, and ends on a sync flush. The compressed piece is kept, with the CRC-32 of the part. A piece is
// valid wherever the same part follows the same predecessor, so a page that is built again is mostly a copy of kept
// pieces. The ETag comes from the SHA-256 digests of the parts, which are also kept.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "app/page_cache.hpp"

namespace campfire::app::splice {

struct Spliced {
  std::string gzip;  // a complete gzip member of the body
  std::string etag;  // the same value as `parts_etag(body, fragments)`
};

// The gzip body and the ETag of `body`, with `fragments` (in order, no overlap) as its parts. Nothing if no fragment
// is big enough to be a part: then use the whole-body path.
[[nodiscard]] std::optional<Spliced> build(std::string_view body, std::span<const FragmentSpan> fragments);

// The CRC-32 of `a ‖ b` from the CRC-32 of `a`, the CRC-32 of `b` and the length of `b` (zlib `crc32_combine`).
[[nodiscard]] std::uint32_t crc32_combine(std::uint32_t crc_a, std::uint32_t crc_b, std::uint64_t length_b) noexcept;

// What the stores keep, for tests and diagnostics.
struct Stats {
  std::size_t pieces = 0;
  std::size_t piece_bytes = 0;
  std::size_t digests = 0;
};
[[nodiscard]] Stats stats();
// Forgets every kept piece and digest (tests).
void clear();

}  // namespace campfire::app::splice
