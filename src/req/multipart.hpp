// multipart/form-data, streaming (Rack: Rack::Multipart::Parser; Rust: crates/kit/src/body.rs
// with the multer crate). The caller feeds chunks of the body. Memory stays bounded: a file part
// goes to a temp file in `tmp_dir` and a text part stays in memory up to a total limit.
// A write to a temp file blocks. The caller runs the parser where a short block is allowed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "req/param.hpp"
#include "req/query.hpp"

namespace campfire::req {

inline constexpr std::size_t kMultipartPartLimit = 4096;               // Rack multipart_total_part_limit
inline constexpr std::size_t kMultipartFileLimit = 128;                // Rack multipart_file_limit
inline constexpr std::size_t kMultipartTextLimit = 16U << 20;          // text fields together: 413 above
inline constexpr std::uint64_t kMultipartBytesizeLimit = 10ULL << 30;  // Rack PARSER_BYTESIZE_LIMIT
inline constexpr std::size_t kMultipartHeaderLimit = 64U << 10;        // the head of one part
inline constexpr std::size_t kMaxBufferedBody = 16U << 20;             // other bodies: 413 above

// The `boundary` of a multipart Content-Type (multer::parse_boundary), or nothing.
[[nodiscard]] std::optional<std::string> parse_boundary(std::string_view content_type);

// The head of a part, as Rack::Multipart::Parser reads it.
struct PartHead {
  std::optional<std::string> name;
  std::optional<std::string> filename;
  std::optional<std::string> content_type;
  std::string head;  // "name: value\r\n" for each header, names in lower case
  // Rack gives a part with no name the file name or "<content type>[]".
  [[nodiscard]] std::string effective_name() const;
};

// Parses a Content-Disposition value (name, filename, filename*).
[[nodiscard]] PartHead parse_disposition(std::string_view value);

class MultipartParser {
 public:
  // `size_limit` is the most bytes of the whole body (the smaller of it and 10 GiB).
  MultipartParser(std::string boundary, std::filesystem::path tmp_dir, std::pmr::memory_resource* mr,
                  std::optional<std::uint64_t> size_limit = std::nullopt);
  ~MultipartParser();
  MultipartParser(const MultipartParser&) = delete;
  MultipartParser& operator=(const MultipartParser&) = delete;

  // Parses a chunk of the body. After an error the parser accepts nothing more.
  [[nodiscard]] ParamResult<void> feed(std::string_view chunk);
  // The end of the body. Gives the params, or the error of an incomplete body.
  [[nodiscard]] ParamResult<ParamMap> finish();

 private:
  enum class State : std::uint8_t { Preamble, AfterBoundary, Headers, Body, Done };
  [[nodiscard]] ParamResult<void> run();
  [[nodiscard]] ParamResult<void> begin_part(std::string_view head_text);
  [[nodiscard]] ParamResult<void> body_data(std::string_view data);
  [[nodiscard]] ParamResult<void> end_part();

  std::string delimiter_;  // "\r\n--" and the boundary
  std::filesystem::path tmp_dir_;
  std::pmr::memory_resource* mr_;
  std::uint64_t size_limit_;
  std::uint64_t total_ = 0;
  std::string buf_;
  State state_ = State::Preamble;
  std::optional<ParamError> failed_;

  std::size_t parts_ = 0;
  std::size_t files_ = 0;
  std::size_t text_ = 0;
  std::vector<RawPair> pairs_;
  // The part that is open.
  PartHead part_;
  enum class Sink : std::uint8_t { Text, File, Discard } sink_ = Sink::Discard;
  std::string text_value_;
  std::shared_ptr<UploadedFile> file_;
  int fd_ = -1;
};

}  // namespace campfire::req
