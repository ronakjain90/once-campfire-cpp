// Thruster's compression: gzhttp with a 1 KB minimum, gzip level 6, zstd level 1 and BREACH jitter.
// Rust: crates/kit/src/front/compression.rs (Thruster: compression_handler.go, compression_guard_handler.go).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "net/http.hpp"

namespace campfire::net::front {

enum class Encoding : std::uint8_t { None, Gzip, Zstd };

inline constexpr std::size_t kMinCompressSize = 1024;
// net/http's `bufferBeforeChunkingSize`: Go sends a longer body with chunked coding.
inline constexpr std::size_t kGoChunkingBuffer = 2048;

// `selectEncoding`: never for HEAD. Zstd when its quality is at least the quality of gzip.
[[nodiscard]] Encoding select_encoding(Method method, std::string_view accept_encoding) noexcept;

// `contentTypeFilter`: gzhttp's default filter, with the images that are already compressed.
[[nodiscard]] bool content_type_filter(std::string_view content_type);

// A part of Go's `http.DetectContentType`, for bodies with no content type.
[[nodiscard]] std::string_view detect_content_type(std::string_view data) noexcept;

// CRC-32C (Castagnoli).
[[nodiscard]] std::uint32_t crc32c(std::string_view data) noexcept;

// `hasUserSpecificRequestHeaders` and `hasUserSpecificResponseHeaders` (GZIP_COMPRESSION_DISABLE_ON_AUTH).
[[nodiscard]] bool has_user_specific_request_headers(const Request& request) noexcept;

// One gzip member as Rack::Deflater writes it (Zlib::GzipWriter): the mtime in the header, OS 3 (Unix).
[[nodiscard]] std::string gzip_member(std::string_view body, std::uint32_t mtime);

struct Negotiation {
  Encoding encoding = Encoding::None;
  bool user_specific_request = false;
};

class Compression {
 public:
  Compression(std::int64_t jitter, bool disable_on_auth);

  [[nodiscard]] Negotiation negotiate(const Request& request) const noexcept;
  [[nodiscard]] bool disable_on_auth() const noexcept { return disable_on_auth_; }

  // The jitter of a body: a prefix of the padding. The length comes from the CRC-32C of the body.
  [[nodiscard]] std::string_view jitter_for(std::string_view body) const noexcept;
  // The whole encoded body, with the jitter. Gzip: a comment in the header. Zstd: a skippable frame.
  [[nodiscard]] std::string encode(Encoding encoding, std::string_view body) const;

 private:
  std::string jitter_;  // "Padding-Padding-..." (n+1 bytes), or empty
  bool disable_on_auth_;
};

}  // namespace campfire::net::front
